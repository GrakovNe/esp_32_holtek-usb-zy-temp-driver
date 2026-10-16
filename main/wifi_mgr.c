/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#include "wifi_mgr.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "captive.h"

static const char *TAG = "wifi-mgr";

#define CONNECT_TIMEOUT_US  (120 * 1000 * 1000)
#define FALLBACK_RETRY_US   (60 * 1000 * 1000)
#define CONNECTING_RETRY_US (2 * 1000 * 1000)
#define SCAN_RESUME_US      (500 * 1000)

typedef enum {
    MSG_SCAN,
    MSG_GOT_IP,
} msg_t;

typedef struct {
    char ssid[APPCONF_SSID_MAX + 1];
    char password[APPCONF_PASS_MAX + 1];
    char ap_ssid[APPCONF_NAME_MAX + 1];
    esp_netif_t *ap_netif;
    esp_netif_t *sta_netif;
    QueueHandle_t queue;

    wifi_mgr_mode_t mode;
    int64_t mode_since_us;
    bool ap_up;
    bool sta_has_ip;
    bool connect_pending;

    bool user_disconnect;
    bool scan_busy;
    int64_t retry_at_us;

    wifi_mgr_status_t status;

    SemaphoreHandle_t scan_lock;
    wifi_mgr_scan_entry_t scan[WIFI_MGR_SCAN_MAX];
    size_t scan_count;
} mgr_t;

static mgr_t s_mgr;

const char *wifi_mgr_mode_name(wifi_mgr_mode_t mode)
{
    switch (mode) {
    case WIFI_MGR_PROVISIONING: return "provisioning";
    case WIFI_MGR_CONNECTING:   return "connecting";
    case WIFI_MGR_NORMAL:       return "normal";
    case WIFI_MGR_FALLBACK:     return "fallback";
    }
    return "?";
}

static const char *disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "wrong_password";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "not_found";
    default:
        return "other";
    }
}

static void set_mode(wifi_mgr_mode_t mode)
{
    if (s_mgr.mode == mode) {
        return;
    }
    ESP_LOGI(TAG, "mode %s -> %s", wifi_mgr_mode_name(s_mgr.mode),
             wifi_mgr_mode_name(mode));
    s_mgr.mode = mode;
    s_mgr.mode_since_us = esp_timer_get_time();
    s_mgr.status.mode = mode;
}

static bool post(msg_t msg)
{
    if (xQueueSend(s_mgr.queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "manager queue full, message %d dropped", msg);
        return false;
    }
    return true;
}

static void ap_start(void)
{
    if (s_mgr.ap_up) {
        return;
    }
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_mgr.ap_ssid, sizeof ap.ap.ssid);
    ap.ap.ssid_len = strlen(s_mgr.ap_ssid);
    ap.ap.channel = 1;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    static char captive_uri[] = CAPTIVE_URL;
    esp_netif_dhcps_stop(s_mgr.ap_netif);
    esp_err_t err = esp_netif_dhcps_option(s_mgr.ap_netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_CAPTIVEPORTAL_URI,
                                           captive_uri, sizeof captive_uri);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dhcp option 114: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_mgr.ap_netif));

    s_mgr.ap_up = true;
    s_mgr.status.ap_up = true;
    captive_start();
    ESP_LOGI(TAG, "AP \"%s\" up, %s", s_mgr.ap_ssid, CAPTIVE_URL);
}

static void ap_stop(void)
{
    if (!s_mgr.ap_up) {
        return;
    }
    captive_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_mgr.ap_up = false;
    s_mgr.status.ap_up = false;
    ESP_LOGI(TAG, "AP off");
}

static void sta_connect(void)
{
    if (s_mgr.ssid[0] == '\0') {
        return;
    }
    if (s_mgr.connect_pending) {

        return;
    }
    wifi_config_t st = { 0 };
    strlcpy((char *)st.sta.ssid, s_mgr.ssid, sizeof st.sta.ssid);
    strlcpy((char *)st.sta.password, s_mgr.password, sizeof st.sta.password);
    st.sta.threshold.authmode =
        s_mgr.password[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_mode(s_mgr.ap_up ? WIFI_MODE_APSTA
                                                  : WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_mode: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &st);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_config(sta): %s", esp_err_to_name(err));
        return;
    }
    s_mgr.user_disconnect = false;
    err = esp_wifi_connect();
    if (err == ESP_OK || err == ESP_ERR_WIFI_CONN) {
        s_mgr.connect_pending = true;
    } else {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

static void handle_scan(void)
{
    /* esp_wifi_scan_start() fails while the STA is connecting. */
    if (s_mgr.ssid[0] != '\0' && !s_mgr.sta_has_ip) {
        s_mgr.user_disconnect = true;
        esp_wifi_disconnect();
    }
    wifi_scan_config_t cfg = { .show_hidden = true };
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start: %s", esp_err_to_name(err));
        s_mgr.user_disconnect = false;
        s_mgr.scan_busy = false;
    }
}

static void on_sta_connected(void *arg, esp_event_base_t base, int32_t id,
                             void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    s_mgr.connect_pending = false;
    ESP_LOGI(TAG, "associated, waiting for IP");
}

static void on_sta_disconnected(void *arg, esp_event_base_t base, int32_t id,
                                void *data)
{
    (void)arg; (void)base; (void)id;
    const wifi_event_sta_disconnected_t *ev = data;
    const char *name = disconnect_reason_name(ev->reason);
    strlcpy(s_mgr.status.last_disconnect, name,
            sizeof s_mgr.status.last_disconnect);
    s_mgr.sta_has_ip = false;
    s_mgr.connect_pending = false;
    if (s_mgr.user_disconnect) {
        ESP_LOGI(TAG, "disconnected (reason %u, ours: scan pause)", ev->reason);
        return;
    }
    ESP_LOGW(TAG, "disconnected (reason %u -> %s)", ev->reason, name);
    int64_t now = esp_timer_get_time();
    s_mgr.retry_at_us = now + (s_mgr.mode == WIFI_MGR_FALLBACK
                                   ? FALLBACK_RETRY_US
                                   : CONNECTING_RETRY_US);
    if (s_mgr.mode == WIFI_MGR_NORMAL) {

        set_mode(WIFI_MGR_CONNECTING);
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const ip_event_got_ip_t *ev = data;
    s_mgr.sta_has_ip = true;
    snprintf(s_mgr.status.ip, sizeof s_mgr.status.ip, IPSTR,
             IP2STR(&ev->ip_info.ip));
    snprintf(s_mgr.status.gateway, sizeof s_mgr.status.gateway, IPSTR,
             IP2STR(&ev->ip_info.gw));
    ESP_LOGI(TAG, "got IP " IPSTR ", gateway " IPSTR,
             IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
    /* Mode change and AP teardown happen on the manager task: ap_stop()
     * waits for the DNS task, and reading ap_up here would race ap_start(). */
    post(MSG_GOT_IP);
}

static void on_scan_done(void *arg, esp_event_base_t base, int32_t id,
                         void *data)
{
    (void)arg; (void)base; (void)id;
    const wifi_event_sta_scan_done_t *ev = data;

    /* static: ~4 KB would overflow the 2304-byte sys_evt stack. */
    static wifi_ap_record_t recs[WIFI_MGR_SCAN_MAX];
    static wifi_mgr_scan_entry_t entries[WIFI_MGR_SCAN_MAX];
    uint16_t n = WIFI_MGR_SCAN_MAX;
    size_t count = 0;

    esp_err_t err = ESP_FAIL;
    if (ev->status == 0) {
        err = esp_wifi_scan_get_ap_records(&n, recs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    } else {

        for (uint16_t i = 0; i < n; i++) {
            if (recs[i].ssid[0] == '\0') {
                continue;
            }
            size_t j;
            for (j = 0; j < count; j++) {
                if (strncmp(entries[j].ssid, (const char *)recs[i].ssid,
                            sizeof entries[j].ssid) == 0) {
                    if (recs[i].rssi > entries[j].rssi) {
                        entries[j].rssi = recs[i].rssi;
                    }
                    break;
                }
            }
            if (j == count && count < WIFI_MGR_SCAN_MAX) {
                strlcpy(entries[count].ssid, (const char *)recs[i].ssid,
                        sizeof entries[count].ssid);
                entries[count].rssi = recs[i].rssi;
                entries[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                count++;
            }
        }

        for (size_t i = 1; i < count; i++) {
            wifi_mgr_scan_entry_t key = entries[i];
            size_t j = i;
            while (j > 0 && entries[j - 1].rssi < key.rssi) {
                entries[j] = entries[j - 1];
                j--;
            }
            entries[j] = key;
        }
        xSemaphoreTake(s_mgr.scan_lock, portMAX_DELAY);
        memcpy(s_mgr.scan, entries, count * sizeof entries[0]);
        s_mgr.scan_count = count;
        xSemaphoreGive(s_mgr.scan_lock);
        ESP_LOGI(TAG, "scan done: %u networks", (unsigned)count);
    }

    s_mgr.scan_busy = false;

    if (s_mgr.ssid[0] != '\0' && !s_mgr.sta_has_ip) {
        s_mgr.retry_at_us = esp_timer_get_time() + SCAN_RESUME_US;
    }
}

static void on_ap_sta(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    const wifi_event_ap_staconnected_t *ev = data;
    ESP_LOGI(TAG, "phone %s AP " MACSTR,
             id == WIFI_EVENT_AP_STACONNECTED ? "joined" : "left",
             MAC2STR(ev->mac));
}

static void mgr_task(void *arg)
{
    (void)arg;
    for (;;) {
        msg_t msg;
        if (xQueueReceive(s_mgr.queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (msg) {
            case MSG_SCAN:
                handle_scan();
                break;
            case MSG_GOT_IP:
                set_mode(WIFI_MGR_NORMAL);
                ap_stop();
                break;
            }
            continue;
        }
        int64_t now = esp_timer_get_time();
        switch (s_mgr.mode) {
        case WIFI_MGR_CONNECTING:
            if (!s_mgr.sta_has_ip) {
                if (now - s_mgr.retry_at_us >= 0) {
                    sta_connect();
                    s_mgr.retry_at_us = now + CONNECTING_RETRY_US;
                }
                if (now - s_mgr.mode_since_us >= CONNECT_TIMEOUT_US) {
                    ap_start();
                    set_mode(WIFI_MGR_FALLBACK);
                    s_mgr.retry_at_us = now + FALLBACK_RETRY_US;
                }
            }
            break;
        case WIFI_MGR_FALLBACK:
            if (!s_mgr.sta_has_ip && now - s_mgr.retry_at_us >= 0) {
                sta_connect();
                s_mgr.retry_at_us = now + FALLBACK_RETRY_US;
            }
            break;
        default:
            break;
        }
    }
}

void wifi_mgr_start(const appconf_t *cfg, const char device_id[5])
{
    mgr_t *m = &s_mgr;
    strlcpy(m->ssid, cfg->wifi_ssid, sizeof m->ssid);
    strlcpy(m->password, cfg->wifi_password, sizeof m->password);
    appconf_default_name(device_id, m->ap_ssid, sizeof m->ap_ssid);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    m->ap_netif = esp_netif_create_default_wifi_ap();
    m->sta_netif = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_netif_set_hostname(m->sta_netif, cfg->hostname));

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, on_sta_connected, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_sta_disconnected, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_SCAN_DONE, on_scan_done, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, on_ap_sta, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, on_ap_sta, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    m->scan_lock = xSemaphoreCreateMutex();
    m->queue = xQueueCreate(8, sizeof(msg_t));
    configASSERT(m->scan_lock != NULL && m->queue != NULL);
    BaseType_t ok = xTaskCreate(mgr_task, "wifi-mgr", 4096, NULL, 5, NULL);
    configASSERT(ok == pdPASS);

    strlcpy(m->status.ssid, cfg->wifi_ssid, sizeof m->status.ssid);
    if (cfg->wifi_ssid[0] == '\0') {
        set_mode(WIFI_MGR_PROVISIONING);
        ap_start();
    } else {
        set_mode(WIFI_MGR_CONNECTING);
        sta_connect();
    }
}

wifi_mgr_mode_t wifi_mgr_mode(void)
{
    return s_mgr.mode;
}

bool wifi_mgr_ap_up(void)
{
    return s_mgr.ap_up;
}

void wifi_mgr_get_status(wifi_mgr_status_t *out)
{
    *out = s_mgr.status;
    out->sta_connected = s_mgr.sta_has_ip;
    if (s_mgr.sta_has_ip) {
        wifi_ap_record_t rec;
        if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
            out->rssi = rec.rssi;
            out->channel = rec.primary;
            snprintf(out->bssid, sizeof out->bssid, MACSTR, MAC2STR(rec.bssid));
        }
    }
}

void wifi_mgr_scan_start(void)
{

    if (s_mgr.scan_busy) {
        return;
    }
    /* Set before posting: the manager task may run and fail the scan
     * before post() returns, and its clear must not be overwritten. */
    s_mgr.scan_busy = true;
    if (!post(MSG_SCAN)) {
        s_mgr.scan_busy = false;
    }
}

bool wifi_mgr_scan_busy(void)
{
    return s_mgr.scan_busy;
}

size_t wifi_mgr_scan_result(wifi_mgr_scan_entry_t *out, size_t max)
{
    xSemaphoreTake(s_mgr.scan_lock, portMAX_DELAY);
    size_t n = s_mgr.scan_count < max ? s_mgr.scan_count : max;
    memcpy(out, s_mgr.scan, n * sizeof out[0]);
    xSemaphoreGive(s_mgr.scan_lock);
    return n;
}
