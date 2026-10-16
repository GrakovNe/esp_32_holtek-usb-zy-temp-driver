/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#include "web_server.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/temperature_sensor.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "captive.h"
#include "fresh.h"
#include "history.h"
#include "mqtt_mgr.h"
#include "reset.h"
#include "settings.h"
#include "time_mgr.h"
#include "wifi_mgr.h"
#include "zytemp.h"

#include "setup_html.h"
#include "main_html.h"
#include "info_html.h"

static const char *TAG = "web";

#define POST_BODY_MAX 8192

static httpd_handle_t s_httpd;
static char s_device_id[5];
static char s_mac_str[18];
static const appconf_t *s_cfg;
static temperature_sensor_handle_t s_tsens;

static double round2(float v)
{
    return round((double)v * 100.0) / 100.0;
}

static void add_str_or_null(cJSON *obj, const char *key, const char *val)
{
    if (val != NULL && val[0] != '\0') {
        cJSON_AddStringToObject(obj, key, val);
    } else {
        cJSON_AddNullToObject(obj, key);
    }
}

static esp_err_t send_json(httpd_req_t *req, int status, const char *status_text,
                           cJSON *body)
{
    httpd_resp_set_status(req, status_text);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char *s = cJSON_PrintUnformatted(body);
    esp_err_t err = ESP_OK;
    if (s != NULL) {
        err = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
        cJSON_free(s);
    } else {
        err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    cJSON_Delete(body);
    return err;
}

static esp_err_t send_json_err(httpd_req_t *req, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    cJSON_AddStringToObject(o, "error", msg);
    return send_json(req, 400, "400 Bad Request", o);
}

static esp_err_t serve_gz(httpd_req_t *req, const unsigned char *gz,
                          unsigned int len)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)gz, len);
}

static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if ((size_t)req->content_len >= cap) {
        return -1;
    }
    size_t total = 0;
    while (total < (size_t)req->content_len) {
        /* httpd_req_recv() can return a partial read. */
        int n = httpd_req_recv(req, buf + total, req->content_len - total);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
    }
    buf[total] = '\0';
    return (int)total;
}

static esp_err_t handler_main(httpd_req_t *req)
{
    return serve_gz(req, main_html_gz, main_html_gz_len);
}

static esp_err_t handler_config(httpd_req_t *req)
{
    return serve_gz(req, setup_html_gz, setup_html_gz_len);
}

static esp_err_t handler_info(httpd_req_t *req)
{
    return serve_gz(req, info_html_gz, info_html_gz_len);
}

static esp_err_t handler_root(httpd_req_t *req)
{
    if (wifi_mgr_ap_up()) {
        return handler_config(req);
    }
    return handler_main(req);
}

static esp_err_t handler_wifi_scan(httpd_req_t *req)
{
    char query[64] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "refresh", val, sizeof val) == ESP_OK) {
            wifi_mgr_scan_start();
        }
    }

    wifi_mgr_scan_entry_t nets[WIFI_MGR_SCAN_MAX];
    size_t n = wifi_mgr_scan_result(nets, WIFI_MGR_SCAN_MAX);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    if (root == NULL || arr == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        if (o == NULL) {
            break;
        }
        cJSON_AddStringToObject(o, "ssid", nets[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", nets[i].rssi);
        cJSON_AddBoolToObject(o, "secure", nets[i].secure);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddBoolToObject(root, "scanning", wifi_mgr_scan_busy());
    return send_json(req, 200, "200 OK", root);
}

static esp_err_t handler_current(httpd_req_t *req)
{
    zytemp_reading_t r = { 0 };
    zytemp_get_reading(&r);

    cJSON *root = cJSON_CreateObject();
    cJSON *sensor = cJSON_AddObjectToObject(root, "sensor");
    if (root == NULL || sensor == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    int64_t ts = time_mgr_unix();
    if (ts < 0) {
        cJSON_AddNullToObject(root, "timestamp");
    } else {
        cJSON_AddNumberToObject(root, "timestamp", (double)ts);
    }
    cJSON_AddBoolToObject(root, "time_synced", time_mgr_synced());
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddBoolToObject(sensor, "connected", r.connected);
    cJSON_AddBoolToObject(sensor, "warming_up", r.co2_warming_up);
    if (co2_fresh(&r)) {
        cJSON *co2 = cJSON_AddObjectToObject(root, "co2");
        cJSON_AddNumberToObject(co2, "ppm", r.co2_ppm);
        cJSON_AddNumberToObject(co2, "age_s", r.co2_age_ms / 1000u);
    } else {
        cJSON_AddNullToObject(root, "co2");
    }
    if (temp_fresh(&r)) {
        cJSON *t = cJSON_AddObjectToObject(root, "temperature");
        cJSON_AddNumberToObject(t, "celsius", round2(r.temp_c));
        cJSON_AddNumberToObject(t, "age_s", r.temp_age_ms / 1000u);
    } else {
        cJSON_AddNullToObject(root, "temperature");
    }
    return send_json(req, 200, "200 OK", root);
}

static esp_err_t handler_history(httpd_req_t *req)
{
    hist_rec_t recs[HIST_RING_CAP];
    uint16_t n = history_snapshot(recs);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    cJSON_AddNumberToObject(root, "step_s", (double)history_step_s());
    cJSON *arr = cJSON_AddArrayToObject(root, "records");
    if (arr == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    for (uint16_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        if (o == NULL) {
            break;
        }
        cJSON_AddNumberToObject(o, "t", recs[i].t);
        if (recs[i].co2_ppm != 0) {
            cJSON_AddNumberToObject(o, "co2", recs[i].co2_ppm);
        } else {
            cJSON_AddNullToObject(o, "co2");
        }
        if (recs[i].temp_c100 != HIST_NO_TEMP) {

            cJSON_AddNumberToObject(o, "temperature",
                                    recs[i].temp_c100 / 100.0);
        } else {
            cJSON_AddNullToObject(o, "temperature");
        }
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, 200, "200 OK", root);
}

static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power_on";
    case ESP_RST_EXT:      return "ext";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
    }
}

static esp_err_t handler_status(httpd_req_t *req)
{
    zytemp_reading_t r = { 0 };
    zytemp_get_reading(&r);
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);

    cJSON *root = cJSON_CreateObject();
    cJSON *tm = cJSON_AddObjectToObject(root, "time");
    cJSON *dev = cJSON_AddObjectToObject(root, "device");
    cJSON *sensor = cJSON_AddObjectToObject(root, "sensor");
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    if (root == NULL || tm == NULL || dev == NULL || sensor == NULL ||
        wifi == NULL || mqtt == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }

    int64_t ts = time_mgr_unix();
    if (ts < 0) {
        cJSON_AddNullToObject(tm, "timestamp");
    } else {
        cJSON_AddNumberToObject(tm, "timestamp", (double)ts);
    }
    cJSON_AddBoolToObject(tm, "synced", time_mgr_synced());
    cJSON_AddNumberToObject(tm, "uptime_s", esp_timer_get_time() / 1000000);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char rev[12];
    snprintf(rev, sizeof rev, "v%u.%u",
             (unsigned)chip.revision / 100, (unsigned)chip.revision % 100);
    uint32_t flash_sz = 0;
    if (esp_flash_get_size(NULL, &flash_sz) != ESP_OK) {
        flash_sz = 0;
    }
    cJSON_AddStringToObject(dev, "name", s_cfg->device_name);
    cJSON_AddStringToObject(dev, "id", s_device_id);
    cJSON_AddStringToObject(dev, "hostname", s_cfg->hostname);
    cJSON_AddStringToObject(dev, "chip", "ESP32-S3");
    cJSON_AddStringToObject(dev, "revision", rev);
    cJSON_AddNumberToObject(dev, "cores", chip.cores);
    cJSON_AddNumberToObject(dev, "cpu_mhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    cJSON_AddStringToObject(dev, "firmware", APP_VER);
    cJSON_AddStringToObject(dev, "idf", esp_get_idf_version());
    cJSON_AddStringToObject(dev, "build_date", __DATE__);
    cJSON_AddStringToObject(dev, "mac", s_mac_str);
    if (flash_sz != 0) {
        cJSON_AddNumberToObject(dev, "flash_bytes", flash_sz);
    } else {
        cJSON_AddNullToObject(dev, "flash_bytes");
    }
    cJSON_AddNumberToObject(dev, "psram_bytes",
                            heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(dev, "heap_free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(dev, "heap_min_free",
                            esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(dev, "reset_reason", reset_reason_str());
    float tc;
    if (s_tsens != NULL &&
        temperature_sensor_get_celsius(s_tsens, &tc) == ESP_OK) {
        cJSON_AddNumberToObject(dev, "chip_temperature_c", round2(tc));
    } else {
        cJSON_AddNullToObject(dev, "chip_temperature_c");
    }

    cJSON_AddStringToObject(root, "mode", wifi_mgr_mode_name(w.mode));

    cJSON_AddBoolToObject(sensor, "connected", r.connected);
    cJSON_AddBoolToObject(sensor, "warming_up", r.co2_warming_up);
    if (r.co2_valid) {
        cJSON_AddNumberToObject(sensor, "co2_ppm", r.co2_ppm);
        cJSON_AddNumberToObject(sensor, "co2_age_s", r.co2_age_ms / 1000u);
    } else {
        cJSON_AddNullToObject(sensor, "co2_ppm");
        cJSON_AddNullToObject(sensor, "co2_age_s");
    }
    if (r.temp_valid) {
        cJSON_AddNumberToObject(sensor, "temperature_c", round2(r.temp_c));
        cJSON_AddNumberToObject(sensor, "temperature_age_s",
                                r.temp_age_ms / 1000u);
    } else {
        cJSON_AddNullToObject(sensor, "temperature_c");
        cJSON_AddNullToObject(sensor, "temperature_age_s");
    }
    cJSON_AddNumberToObject(sensor, "frames_valid", r.frames_valid);
    cJSON_AddNumberToObject(sensor, "frames_bad", r.frames_bad);

    cJSON_AddBoolToObject(wifi, "connected", w.sta_connected);
    add_str_or_null(wifi, "ssid", w.ssid);
    add_str_or_null(wifi, "bssid", w.bssid);
    if (w.sta_connected) {
        cJSON_AddNumberToObject(wifi, "rssi", w.rssi);
        cJSON_AddNumberToObject(wifi, "channel", w.channel);
        add_str_or_null(wifi, "ip", w.ip);
        add_str_or_null(wifi, "gateway", w.gateway);
    } else {
        cJSON_AddNullToObject(wifi, "rssi");
        cJSON_AddNullToObject(wifi, "channel");
        cJSON_AddNullToObject(wifi, "ip");
        cJSON_AddNullToObject(wifi, "gateway");
    }
    add_str_or_null(wifi, "last_disconnect", w.last_disconnect);

    mqtt_mgr_status_t mq;
    mqtt_mgr_get_status(&mq);
    cJSON_AddBoolToObject(mqtt, "enabled", mq.enabled);
    cJSON_AddBoolToObject(mqtt, "connected", mq.connected);
    if (mq.last_publish > 0) {
        cJSON_AddNumberToObject(mqtt, "last_publish", (double)mq.last_publish);
    } else {
        cJSON_AddNullToObject(mqtt, "last_publish");
    }
    cJSON_AddNumberToObject(mqtt, "errors", mq.errors);

    return send_json(req, 200, "200 OK", root);
}

static esp_err_t handler_config_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *dev = cJSON_AddObjectToObject(root, "device");
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON *time = cJSON_AddObjectToObject(root, "time");
    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    if (root == NULL || dev == NULL || wifi == NULL || time == NULL ||
        mqtt == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    cJSON_AddStringToObject(dev, "name", s_cfg->device_name);
    cJSON_AddStringToObject(wifi, "ssid", s_cfg->wifi_ssid);

    cJSON_AddBoolToObject(wifi, "password_set", s_cfg->wifi_password[0] != '\0');
    cJSON_AddStringToObject(time, "ntp_server", s_cfg->ntp_server);
    cJSON_AddBoolToObject(mqtt, "enabled", s_cfg->mqtt_enabled);
    cJSON_AddStringToObject(mqtt, "host", s_cfg->mqtt_host);
    cJSON_AddNumberToObject(mqtt, "port", s_cfg->mqtt_port);
    cJSON_AddStringToObject(mqtt, "username", s_cfg->mqtt_username);
    cJSON_AddBoolToObject(mqtt, "password_set", s_cfg->mqtt_password[0] != '\0');
    cJSON_AddBoolToObject(mqtt, "tls", s_cfg->mqtt_tls);
    cJSON_AddBoolToObject(mqtt, "ca_cert_set", s_cfg->mqtt_ca_cert[0] != '\0');
    cJSON_AddBoolToObject(mqtt, "skip_verify", s_cfg->mqtt_skip_verify);
    cJSON_AddStringToObject(mqtt, "co2_topic", s_cfg->mqtt_co2_topic);
    cJSON_AddStringToObject(mqtt, "temperature_topic", s_cfg->mqtt_temp_topic);
    cJSON_AddNumberToObject(mqtt, "interval_s", s_cfg->mqtt_interval_s);
    cJSON_AddBoolToObject(mqtt, "discovery", s_cfg->mqtt_discovery);
    return send_json(req, 200, "200 OK", root);
}

static void restart_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "restarting after config save");
    esp_restart();
}

/* Restart after the response has left: a restart inside the handler
 * would cut the connection before the client sees the answer. */
static void schedule_restart(void)
{

    const esp_timer_create_args_t args = {
        .callback = restart_timer_cb,
        .name = "cfgrestart",
    };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 1000 * 1000);
    } else {
        esp_restart();
    }
}

static bool merge_str(const cJSON *obj, const char *key, char *dst, size_t cap,
                      char *err, size_t err_sz)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNull(v)) {
        snprintf(err, err_sz, "\"%s\" must be a string", key);
        return false;
    }
    if (v == NULL) {
        return true;
    }
    if (!cJSON_IsString(v) || strnlen(v->valuestring, cap + 1) > cap) {
        snprintf(err, err_sz, "\"%s\" must be a string of at most %u bytes",
                 key, (unsigned)cap);
        return false;
    }
    strcpy(dst, v->valuestring);
    return true;
}

static bool merge_bool(const cJSON *obj, const char *key, bool *dst,
                       char *err, size_t err_sz)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return true;
    }
    if (!cJSON_IsBool(v)) {
        snprintf(err, err_sz, "\"%s\" must be true or false", key);
        return false;
    }
    *dst = cJSON_IsTrue(v);
    return true;
}

static bool merge_int(const cJSON *obj, const char *key, int32_t *dst,
                      int min, int max, char *err, size_t err_sz)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        return true;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble < min || v->valuedouble > max ||
        v->valuedouble != floor(v->valuedouble)) {
        snprintf(err, err_sz, "\"%s\" must be an integer between %d and %d",
                 key, min, max);
        return false;
    }
    *dst = (int32_t)v->valuedouble;
    return true;
}

static esp_err_t handler_config_post(httpd_req_t *req)
{
    char *body = malloc(POST_BODY_MAX);
    if (body == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    int len = read_body(req, body, POST_BODY_MAX);
    if (len < 0) {
        free(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return send_json_err(req, "body is not valid JSON");
    }

    appconf_t merged;
    settings_load(&merged, s_device_id);

    char err[128] = "";
    const cJSON *dev = cJSON_GetObjectItemCaseSensitive(root, "device");
    if (dev != NULL && !cJSON_IsObject(dev)) {
        snprintf(err, sizeof err, "\"device\" must be an object");
    } else if (dev != NULL) {
        merge_str(dev, "name", merged.device_name, sizeof merged.device_name,
                  err, sizeof err);
    }

    if (err[0] == '\0') {
        const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
        if (wifi != NULL && !cJSON_IsObject(wifi)) {
            snprintf(err, sizeof err, "\"wifi\" must be an object");
        } else if (wifi != NULL) {
            if (merge_str(wifi, "ssid", merged.wifi_ssid,
                          sizeof merged.wifi_ssid, err, sizeof err)) {
                merge_str(wifi, "password", merged.wifi_password,
                          sizeof merged.wifi_password, err, sizeof err);
            }
        }
    }

    if (err[0] == '\0') {
        const cJSON *tm = cJSON_GetObjectItemCaseSensitive(root, "time");
        if (tm != NULL && !cJSON_IsObject(tm)) {
            snprintf(err, sizeof err, "\"time\" must be an object");
        } else if (tm != NULL) {
            merge_str(tm, "ntp_server", merged.ntp_server,
                      sizeof merged.ntp_server, err, sizeof err);
        }
    }

    if (err[0] == '\0') {
        const cJSON *mq = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
        if (mq != NULL && !cJSON_IsObject(mq)) {
            snprintf(err, sizeof err, "\"mqtt\" must be an object");
        } else if (mq != NULL) {

            int32_t port = merged.mqtt_port;
            int32_t interval = merged.mqtt_interval_s;
            bool ok = merge_bool(mq, "enabled", &merged.mqtt_enabled,
                                 err, sizeof err) &&
                      merge_str(mq, "host", merged.mqtt_host,
                                sizeof merged.mqtt_host, err, sizeof err) &&
                      merge_int(mq, "port", &port, 1, 65535, err, sizeof err) &&
                      merge_str(mq, "username", merged.mqtt_username,
                                sizeof merged.mqtt_username, err, sizeof err) &&
                      merge_str(mq, "password", merged.mqtt_password,
                                sizeof merged.mqtt_password, err, sizeof err) &&
                      merge_bool(mq, "tls", &merged.mqtt_tls, err, sizeof err) &&
                      merge_str(mq, "ca_cert", merged.mqtt_ca_cert,
                                sizeof merged.mqtt_ca_cert, err, sizeof err) &&
                      merge_bool(mq, "skip_verify", &merged.mqtt_skip_verify,
                                 err, sizeof err) &&
                      merge_str(mq, "co2_topic", merged.mqtt_co2_topic,
                                sizeof merged.mqtt_co2_topic, err, sizeof err) &&
                      merge_str(mq, "temperature_topic",
                                merged.mqtt_temp_topic,
                                sizeof merged.mqtt_temp_topic, err, sizeof err) &&
                      merge_int(mq, "interval_s", &interval,
                                APPCONF_MQTT_INTERVAL_MIN_S,
                                APPCONF_MQTT_INTERVAL_MAX_S, err, sizeof err) &&
                      merge_bool(mq, "discovery", &merged.mqtt_discovery,
                                 err, sizeof err);
            if (ok) {
                merged.mqtt_port = (uint16_t)port;
                merged.mqtt_interval_s = (uint16_t)interval;
            }
        }
    }

    if (err[0] == '\0') {
        appconf_validate(&merged, err, sizeof err);
    }

    if (err[0] != '\0') {
        cJSON_Delete(root);
        return send_json_err(req, err);
    }

    esp_err_t err2 = settings_save(&merged);
    cJSON_Delete(root);
    if (err2 != ESP_OK) {
        return send_json_err(req, "could not save settings");
    }

    appconf_derive(&merged, s_device_id);
    cJSON *out = cJSON_CreateObject();
    char url[APPCONF_HOST_MAX + 32];
    snprintf(url, sizeof url, "http://%s.local/", merged.hostname);
    if (out == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "url", url);
    ESP_LOGI(TAG, "settings saved, restarting soon");
    esp_err_t r = send_json(req, 200, "200 OK", out);
    schedule_restart();
    return r;
}

static void drain_body(httpd_req_t *req)
{

    char scratch[64];
    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, scratch,
                               remaining < (int)sizeof scratch
                                   ? remaining : (int)sizeof scratch);
        if (n <= 0) {
            return;
        }
        remaining -= n;
    }
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, 200, "200 OK", o);
}

static esp_err_t handler_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods",
                       "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handler_factory_reset(httpd_req_t *req)
{
    drain_body(req);
    ESP_LOGW(TAG, "factory reset requested over HTTP");
    esp_err_t r = send_ok(req);
    reset_mark_factory();
    schedule_restart();
    return r;
}

static esp_err_t handler_reboot(httpd_req_t *req)
{
    drain_body(req);
    ESP_LOGW(TAG, "reboot requested over HTTP");
    esp_err_t r = send_ok(req);
    schedule_restart();
    return r;
}

static esp_err_t handler_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    if (wifi_mgr_ap_up()) {
        static const char body[] =
            "<html><head><meta http-equiv=\"refresh\" content=\"0;url=" CAPTIVE_URL "\"></head>"
            "<body>Setting up your device: <a href=\"" CAPTIVE_URL "\">AirMonitor setup</a></body></html>";
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", CAPTIVE_URL);
        httpd_resp_set_type(req, "text/html; charset=utf-8");

        return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "not found", HTTPD_RESP_USE_STRLEN);
}

void web_server_start(const appconf_t *cfg)
{
    s_cfg = cfg;
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    appconf_device_id(mac, s_device_id);
    snprintf(s_mac_str, sizeof s_mac_str, MACSTR, MAC2STR(mac));

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 16;
    hc.stack_size = 8192;
    hc.lru_purge_enable = true;
    hc.server_port = 80;

    ESP_ERROR_CHECK(httpd_start(&s_httpd, &hc));

    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = handler_root },
        { .uri = "/main", .method = HTTP_GET, .handler = handler_main },
        { .uri = "/config", .method = HTTP_GET, .handler = handler_config },
        { .uri = "/info", .method = HTTP_GET, .handler = handler_info },
        { .uri = "/api/v1/wifi/scan", .method = HTTP_GET, .handler = handler_wifi_scan },
        { .uri = "/api/v1/current", .method = HTTP_GET, .handler = handler_current },
        { .uri = "/api/v1/history", .method = HTTP_GET, .handler = handler_history },
        { .uri = "/api/v1/status", .method = HTTP_GET, .handler = handler_status },
        { .uri = "/api/v1/config", .method = HTTP_GET, .handler = handler_config_get },
        { .uri = "/api/v1/config", .method = HTTP_POST, .handler = handler_config_post },
        { .uri = "/api/v1/config", .method = HTTP_OPTIONS, .handler = handler_options },
        { .uri = "/api/v1/factory-reset", .method = HTTP_POST, .handler = handler_factory_reset },
        { .uri = "/api/v1/factory-reset", .method = HTTP_OPTIONS, .handler = handler_options },
        { .uri = "/api/v1/reboot", .method = HTTP_POST, .handler = handler_reboot },
        { .uri = "/api/v1/reboot", .method = HTTP_OPTIONS, .handler = handler_options },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &uris[i]));
    }
    ESP_ERROR_CHECK(httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND,
                                               handler_404));

    const temperature_sensor_config_t tcfg =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &s_tsens) != ESP_OK) {
        s_tsens = NULL;
    } else if (temperature_sensor_enable(s_tsens) != ESP_OK) {
        s_tsens = NULL;
    }

    ESP_LOGI(TAG, "http server up on :80");
}
