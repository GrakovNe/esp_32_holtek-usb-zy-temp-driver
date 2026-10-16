/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#include "selftest.h"

#include "sdkconfig.h"

#if CONFIG_APP_SELFTEST

#include <lwip/sockets.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "appconf.h"
#include "captive.h"
#include "time_mgr.h"
#include "wifi_mgr.h"

static const char *TAG = "selftest";

#define RESP_MAX 2048
#define RECV_TIMEOUT_MS 400

static int s_pass;
static int s_fail;

static char s_id[5];
static char s_fqdn_default[40];
static char s_topic_avail[48];
static char s_topic_co2[48];
static char s_topic_temp[48];
static char s_disc_co2[80];
static char s_disc_temp[80];
static char s_want_unique_co2[64];
static char s_want_state_temp[80];

static void build_names(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    appconf_device_id(mac, s_id);
    snprintf(s_fqdn_default, sizeof s_fqdn_default, "airmonitor-%s.local", s_id);
    snprintf(s_topic_avail, sizeof s_topic_avail, "airmonitor/%s/availability", s_id);
    snprintf(s_topic_co2, sizeof s_topic_co2, "airmonitor/%s/co2", s_id);
    snprintf(s_topic_temp, sizeof s_topic_temp, "airmonitor/%s/temperature", s_id);
    snprintf(s_disc_co2, sizeof s_disc_co2,
             "homeassistant/sensor/airmonitor_%s_co2/config", s_id);
    snprintf(s_disc_temp, sizeof s_disc_temp,
             "homeassistant/sensor/airmonitor_%s_temperature/config", s_id);
    snprintf(s_want_unique_co2, sizeof s_want_unique_co2,
             "\"unique_id\":\"airmonitor_%s_co2\"", s_id);
    snprintf(s_want_state_temp, sizeof s_want_state_temp,
             "\"state_topic\":\"airmonitor/%s/temperature\"", s_id);
}

#ifdef SELFTEST_MQTT
#define ST_MQTT_HOST     "127.0.0.1"
#define ST_MQTT_PORT     "1883"
#define ST_MQTT_INTERVAL "5"
#define ST_MQTT_DISC     "true"
#else
#define ST_MQTT_HOST     "mqtt.test"
#define ST_MQTT_PORT     "1884"
#define ST_MQTT_INTERVAL "45"
#define ST_MQTT_DISC     "false"
#endif

static void report(bool ok, const char *name, const char *detail)
{
    if (ok) {
        s_pass++;
        ESP_LOGI(TAG, "SELFTEST PASS %s", name);
    } else {
        s_fail++;
        ESP_LOGE(TAG, "SELFTEST FAIL %s: %s", name, detail ? detail : "?");
    }
}

static bool request(const char *method, const char *path, const char *body,
                    char *resp, size_t resp_sz)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        return false;
    }
    struct timeval tv = { .tv_sec = RECV_TIMEOUT_MS / 1000,
                          .tv_usec = (RECV_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
        .sin_addr.s_addr = htonl(0x7F000001),
    };
    bool ok = false;
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
        goto out;
    }

    char req[256];
    int n;
    if (body != NULL) {
        n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.1\r\nHost: selftest\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %u\r\nConnection: close\r\n\r\n",
                     method, path, (unsigned)strlen(body));
    } else {
        n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.1\r\nHost: selftest\r\n"
                     "Connection: close\r\n\r\n",
                     method, path);
    }
    if (n < 0 || (size_t)n >= sizeof req) {
        goto out;
    }
    if (send(s, req, (size_t)n, 0) < 0) {
        goto out;
    }
    if (body != NULL) {
        size_t sent = 0;
        while (sent < strlen(body)) {
            int w = send(s, body + sent, strlen(body) - sent, 0);
            if (w <= 0) {
                goto out;
            }
            sent += (size_t)w;
        }
    }

    size_t got = 0;
    int empty_reads = 0;
    while (got < resp_sz - 1) {
        int r = recv(s, resp + got, resp_sz - 1 - got, 0);
        if (r > 0) {
            got += (size_t)r;
            continue;
        }
        if (r == 0) {
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (got > 0) {
                break;
            }

            if (++empty_reads > 25) {
                break;
            }
            continue;
        }
        break;
    }
    resp[got] = '\0';
    ok = got >= 12 && strncmp(resp, "HTTP/1.1 ", 9) == 0;

out:
    close(s);
    return ok;
}

static int status_of(const char *resp)
{
    int status = 0;
    sscanf(resp, "HTTP/1.1 %d", &status);
    return status;
}

static bool header_of(const char *resp, const char *name,
                      char *val, size_t val_sz)
{
    val[0] = '\0';
    const char *p = resp;
    size_t nlen = strlen(name);
    while ((p = strstr(p, "\r\n")) != NULL) {
        p += 2;
        if (p[0] == '\r' && p[1] == '\n') {
            break;
        }
        if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ') {
                v++;
            }
            const char *e = strstr(v, "\r\n");
            if (e == NULL) {
                return false;
            }
            size_t len = (size_t)(e - v);
            if (len >= val_sz) {
                len = val_sz - 1;
            }
            memcpy(val, v, len);
            val[len] = '\0';
            return true;
        }
    }
    return false;
}

static void check(const char *name, const char *method, const char *path,
                  const char *body, int want_status, const char *want_substr)
{
    char *resp = malloc(RESP_MAX);
    if (resp == NULL) {
        report(false, name, "no memory");
        return;
    }
    if (!request(method, path, body, resp, RESP_MAX)) {
        report(false, name, "transport error");
        free(resp);
        return;
    }
    int status = status_of(resp);
    if (status != want_status) {
        char d[64];
        snprintf(d, sizeof d, "status %d != %d", status, want_status);
        report(false, name, d);
        ESP_LOGE(TAG, "%.200s", resp);
        free(resp);
        return;
    }
    if (want_substr != NULL && strstr(resp, want_substr) == NULL) {
        report(false, name, "response missing substring");
        ESP_LOGE(TAG, "%.200s", resp);
        free(resp);
        return;
    }
    report(true, name, NULL);
    free(resp);
}

/* A header-only check passes even when the embedded page is a 0-byte
 * array, so the body is checked for the gzip magic and a real length. */
static void check_gz_body(const char *name, const char *path)
{
    char *resp = malloc(RESP_MAX);
    if (resp == NULL) {
        report(false, name, "no memory");
        return;
    }
    if (!request("GET", path, NULL, resp, RESP_MAX)) {
        report(false, name, "transport error");
        free(resp);
        return;
    }
    const char *hdr_end = strstr(resp, "\r\n\r\n");
    const char *cl = strstr(resp, "Content-Length: ");
    long clen = (cl != NULL && hdr_end != NULL && cl < hdr_end)
                    ? strtol(cl + 16, NULL, 10)
                    : -1;
    const unsigned char *body =
        hdr_end != NULL ? (const unsigned char *)hdr_end + 4 : NULL;
    char d[64];
    if (clen < 1000 || body == NULL || body[0] != 0x1f || body[1] != 0x8b) {
        snprintf(d, sizeof d, "clen %ld, body %02x %02x", clen,
                 body ? body[0] : 0, body ? body[1] : 0);
        report(false, name, d);
    } else {
        report(true, name, NULL);
    }
    free(resp);
}

static void check_cors(const char *name, const char *path)
{
    char *resp = malloc(RESP_MAX);
    if (resp == NULL) {
        report(false, name, "no memory");
        return;
    }
    char val[32];
    bool ok = request("GET", path, NULL, resp, RESP_MAX) &&
              header_of(resp, "Access-Control-Allow-Origin", val, sizeof val) &&
              strcmp(val, "*") == 0;
    report(ok, name, val);
    free(resp);
}

static bool mdns_ask(const char *name, uint16_t qtype,
                     const uint8_t *want, size_t want_sz)
{
    uint8_t q[72];
    size_t n = 0;
    q[n++] = 0; q[n++] = 0;
    q[n++] = 0; q[n++] = 0;
    q[n++] = 0; q[n++] = 1;
    q[n++] = 0; q[n++] = 0;
    q[n++] = 0; q[n++] = 0;
    q[n++] = 0; q[n++] = 0;
    for (const char *p = name; *p != '\0';) {
        const char *dot = strchr(p, '.');
        size_t len = dot != NULL ? (size_t)(dot - p) : strlen(p);
        if (len == 0 || len > 63 || n + len + 1 > sizeof q) {
            return false;
        }
        q[n++] = (uint8_t)len;
        memcpy(&q[n], p, len);
        n += len;
        p += len + (dot != NULL ? 1 : 0);
    }
    q[n++] = 0;
    q[n++] = (uint8_t)(qtype >> 8);
    q[n++] = (uint8_t)qtype;
    q[n++] = 0;
    q[n++] = 1;

    bool ok = false;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return false;
    }
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(5353),
        .sin_addr.s_addr = htonl(0xC0A80401),
    };
    uint8_t r[512];
    int got = -1;
    if (sendto(s, q, n, 0, (struct sockaddr *)&sa, sizeof sa) == (int)n) {

        got = recv(s, r, sizeof r, 0);
    }
    int err = errno;
    close(s);
    if (got <= 12) {
        ESP_LOGW(TAG, "mdns_ask %s: recv %d, errno %d", name, got, err);
        return false;
    }
    ok = (r[2] & 0x80) != 0 && ((r[6] << 8) | r[7]) > 0;
    if (ok) {
        bool found = false;
        for (int i = 0; i + (int)want_sz <= got; i++) {
            if (memcmp(&r[i], want, want_sz) == 0) {
                found = true;
                break;
            }
        }
        ok = found;
    }
    return ok;
}

#ifdef SELFTEST_NTP

#define NTP_TEST_EPOCH 1792000000LL
#define NTP_UNIX_TO_NTP 2208988800ULL

static void ntp_responder_task(void *arg)
{
    int s = (int)(intptr_t)arg;
    for (;;) {
        uint8_t req[48];
        struct sockaddr_in from;
        socklen_t from_sz = sizeof from;
        int r = recvfrom(s, req, sizeof req, 0, (struct sockaddr *)&from,
                         &from_sz);
        if (r < 48) {
            continue;
        }
        uint8_t rsp[48] = { 0 };
        rsp[0] = 0x24;
        rsp[1] = 2;
        rsp[2] = 4;
        rsp[3] = 0xE8;
        memcpy(&rsp[24], &req[40], 8);
        uint32_t sec = htonl((uint32_t)(NTP_UNIX_TO_NTP + NTP_TEST_EPOCH +
                                        esp_timer_get_time() / 1000000));
        memcpy(&rsp[32], &sec, 4);
        memcpy(&rsp[40], &sec, 4);
        sendto(s, rsp, sizeof rsp, 0, (struct sockaddr *)&from, from_sz);
    }
}

static void ntp_responder_start(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        ESP_LOGE(TAG, "ntp test server: socket failed");
        return;
    }
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(123),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
        ESP_LOGE(TAG, "ntp test server: bind :123 failed");
        close(s);
        return;
    }
    if (xTaskCreate(ntp_responder_task, "ntptest", 3072,
                    (void *)(intptr_t)s, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "ntp test server: no task memory");
        close(s);
    }
}
#endif

#ifdef SELFTEST_MQTT

#define BRK_BUF 1024

static struct {
    volatile bool connected;
    char will_topic[80];
    char will_msg[40];
    volatile bool avail_online;
    volatile bool avail_retain;
    volatile bool disc_co2;
    volatile bool disc_co2_retain;
    volatile bool disc_temp;
    volatile bool sub_ha_status;
    char pub_co2[24];
    char pub_temp[24];
} brk;

static bool read_full(int s, uint8_t *p, size_t n)
{
    while (n > 0) {
        int r = recv(s, p, n, 0);
        if (r <= 0) {
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool read_varint(int s, size_t *val)
{
    size_t v = 0;
    for (int shift = 0; shift < 28; shift += 7) {
        uint8_t b;
        if (!read_full(s, &b, 1)) {
            return false;
        }
        v |= (size_t)(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            *val = v;
            return true;
        }
    }
    return false;
}

static bool str_field(const uint8_t *buf, size_t len, size_t *pos, char *out,
                      size_t out_sz)
{
    if (*pos + 2 > len) {
        return false;
    }
    size_t n = ((size_t)buf[*pos] << 8) | buf[*pos + 1];
    *pos += 2;
    if (*pos + n > len) {
        return false;
    }
    if (out != NULL) {
        size_t c = n < out_sz - 1 ? n : out_sz - 1;
        memcpy(out, buf + *pos, c);
        out[c] = '\0';
    }
    *pos += n;
    return true;
}

static void record_publish(const char *topic, const char *payload, bool retain)
{
    ESP_LOGI(TAG, "broker PUBLISH %s: %.80s", topic, payload);
    if (strcmp(topic, s_topic_avail) == 0) {
        if (strcmp(payload, "online") == 0) {
            brk.avail_online = true;
            brk.avail_retain = retain;
        }
    } else if (strcmp(topic, s_disc_co2) == 0) {
        if (strstr(payload, "\"device_class\":\"carbon_dioxide\"") != NULL &&
            strstr(payload, s_want_unique_co2) != NULL &&
            strstr(payload, "availability_topic") != NULL) {
            brk.disc_co2 = true;
            brk.disc_co2_retain = retain;
        }
    } else if (strcmp(topic, s_disc_temp) == 0) {
        if (strstr(payload, "\"device_class\":\"temperature\"") != NULL &&
            strstr(payload, s_want_state_temp) != NULL) {
            brk.disc_temp = true;
        }
    } else if (strcmp(topic, s_topic_co2) == 0) {
        snprintf(brk.pub_co2, sizeof brk.pub_co2, "%.20s", payload);
    } else if (strcmp(topic, s_topic_temp) == 0) {
        snprintf(brk.pub_temp, sizeof brk.pub_temp, "%.20s", payload);
    }
}

static void broker_conn(int s)
{
    uint8_t *buf = malloc(BRK_BUF);
    if (buf == NULL) {
        return;
    }
    for (;;) {
        uint8_t hdr;
        if (!read_full(s, &hdr, 1)) {
            break;
        }
        size_t len = 0;
        if (!read_varint(s, &len) || len > BRK_BUF - 1) {
            break;
        }
        if (len > 0 && !read_full(s, buf, len)) {
            break;
        }
        buf[len] = '\0';

        uint8_t type = hdr >> 4;
        if (type == 1) {
            if (len < 8) {
                break;
            }
            size_t pn = ((size_t)buf[0] << 8) | buf[1];
            if (pn + 6 > len) {
                break;
            }
            uint8_t flags = buf[pn + 3];
            size_t pos = pn + 6;
            if (!str_field(buf, len, &pos, NULL, 0)) {
                break;
            }
            brk.will_topic[0] = '\0';
            brk.will_msg[0] = '\0';
            if ((flags & 0x04) != 0 &&
                (!str_field(buf, len, &pos, brk.will_topic,
                            sizeof brk.will_topic) ||
                 !str_field(buf, len, &pos, brk.will_msg,
                            sizeof brk.will_msg))) {
                break;
            }
            if ((flags & 0x80) != 0 && !str_field(buf, len, &pos, NULL, 0)) {
                break;
            }
            if ((flags & 0x40) != 0 && !str_field(buf, len, &pos, NULL, 0)) {
                break;
            }
            brk.connected = true;
            ESP_LOGI(TAG, "broker: CONNECT, will %s = %s", brk.will_topic,
                     brk.will_msg);
            const uint8_t connack[4] = { 0x20, 0x02, 0x00, 0x00 };
            send(s, connack, sizeof connack, 0);
        } else if (type == 8) {
            if (len < 2) {
                break;
            }
            char topic[64];
            size_t pos = 2;
            if (str_field(buf, len, &pos, topic, sizeof topic)) {
                ESP_LOGI(TAG, "broker SUBSCRIBE %s", topic);
                if (strcmp(topic, "homeassistant/status") == 0) {
                    brk.sub_ha_status = true;
                }
            }
            const uint8_t suback[5] = { 0x90, 0x03, buf[0], buf[1], 0x00 };
            send(s, suback, sizeof suback, 0);
        } else if (type == 3) {
            uint8_t qos = (hdr >> 1) & 3;
            bool retain = (hdr & 1) != 0;
            char topic[100];
            size_t pos = 0;
            if (!str_field(buf, len, &pos, topic, sizeof topic)) {
                break;
            }
            uint16_t pid = 0;
            if (qos > 0) {
                if (pos + 2 > len) {
                    break;
                }
                pid = (uint16_t)((buf[pos] << 8) | buf[pos + 1]);
                pos += 2;
            }
            size_t plen = len - pos;
            char *payload = malloc(plen + 1);
            if (payload == NULL) {
                break;
            }
            memcpy(payload, buf + pos, plen);
            payload[plen] = '\0';
            record_publish(topic, payload, retain);
            free(payload);
            if (qos == 1) {
                const uint8_t puback[4] = { 0x40, 0x02, (uint8_t)(pid >> 8),
                                            (uint8_t)pid };
                send(s, puback, sizeof puback, 0);
            }
        } else if (type == 12) {
            const uint8_t pingresp[2] = { 0xD0, 0x00 };
            send(s, pingresp, sizeof pingresp, 0);
        } else if (type == 14) {
            break;
        }
    }
    free(buf);
}

static void broker_task(void *arg)
{
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls < 0) {
        ESP_LOGE(TAG, "broker: socket failed");
        return;
    }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(1883),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(ls, 2) < 0) {
        ESP_LOGE(TAG, "broker: bind/listen :1883 failed");
        close(ls);
        return;
    }
    ESP_LOGI(TAG, "broker listening on :1883");
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        broker_conn(c);
        close(c);
    }
}

static bool all_digits(const char *s, bool allow_dot)
{
    if (s[0] == '\0') {
        return false;
    }
    bool dot = false;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '.') {
            if (!allow_dot || dot) {
                return false;
            }
            dot = true;
        } else if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

static bool wait_flag(volatile bool *cond, int ms)
{
    for (int i = 0; i < ms / 500; i++) {
        if (*cond) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return *cond;
}

static void mqtt_phase(void)
{

    report(wait_flag(&brk.connected, 40000), "mqtt_connect",
           "no CONNECT within 40 s");
    if (!brk.connected) {
        return;
    }
    char d[160];
    bool will_ok = strcmp(brk.will_topic, s_topic_avail) == 0 &&
                   strcmp(brk.will_msg, "offline") == 0;
    snprintf(d, sizeof d, "will %s = %s", brk.will_topic, brk.will_msg);
    report(will_ok, "mqtt_will", d);

    report(wait_flag(&brk.avail_online, 15000), "mqtt_availability",
           "no retained online within 15 s");
    report(brk.avail_retain, "mqtt_availability_retained", "retain flag 0");

    report(wait_flag(&brk.disc_co2, 15000), "mqtt_discovery_co2",
           "no valid CO2 config within 15 s");
    report(brk.disc_co2_retain, "mqtt_discovery_co2_retained", "retain flag 0");
    report(wait_flag(&brk.disc_temp, 5000), "mqtt_discovery_temp",
           "no valid temperature config within 5 s");

    report(wait_flag(&brk.sub_ha_status, 5000), "mqtt_subscribe_ha_status",
           "no SUBSCRIBE homeassistant/status within 5 s");

    for (int i = 0; i < 120 && (brk.pub_co2[0] == '\0' ||
                                brk.pub_temp[0] == '\0'); i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    snprintf(d, sizeof d, "co2=\"%s\" temp=\"%s\"", brk.pub_co2, brk.pub_temp);
    report(all_digits(brk.pub_temp, true), "mqtt_publish_temp", d);
    report(all_digits(brk.pub_co2, false), "mqtt_publish_co2", d);

    char *st = malloc(RESP_MAX);
    bool st_ok = st != NULL && request("GET", "/api/v1/status", NULL, st,
                                       RESP_MAX) &&
                 strstr(st, "\"connected\":true") != NULL &&
                 strstr(st, "\"last_publish\":179") != NULL;
    report(st_ok, "mqtt_status_api", st != NULL ? st : "no memory");
    free(st);
}
#endif

static void selftest_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGW(TAG, "SELFTEST begin");

    check("root_200", "GET", "/", NULL, 200, "Content-Encoding: gzip");
    check("main_200", "GET", "/main", NULL, 200, "Content-Encoding: gzip");
    check("config_page_200", "GET", "/config", NULL, 200,
          "Content-Encoding: gzip");
    check_gz_body("root_gz_body", "/");
    check_gz_body("main_gz_body", "/main");
    check_gz_body("config_gz_body", "/config");
    check("info_page_200", "GET", "/info", NULL, 200, "Content-Encoding: gzip");
    check_gz_body("info_gz_body", "/info");
    if (wifi_mgr_ap_up()) {
        check("captive_404_303", "GET", "/generate_204", NULL, 303,
              "Location: " CAPTIVE_URL);
    } else {
        check("plain_404", "GET", "/generate_204", NULL, 404, NULL);
    }
    check("api_config_200", "GET", "/api/v1/config", NULL, 200,
          "\"password_set\"");
    check("api_config_ntp", "GET", "/api/v1/config", NULL, 200,
          "\"ntp_server\"");
    check("api_config_mqtt", "GET", "/api/v1/config", NULL, 200,
          "\"ca_cert_set\"");
    check("api_current_200", "GET", "/api/v1/current", NULL, 200,
          "\"time_synced\"");
    check("api_current_sensor", "GET", "/api/v1/current", NULL, 200,
          "\"warming_up\"");
    check("api_status_200", "GET", "/api/v1/status", NULL, 200,
          "\"firmware\"");
    check("api_status_wifi", "GET", "/api/v1/status", NULL, 200,
          "\"last_disconnect\"");
    {

        char *r = malloc(RESP_MAX);
        bool ok = r != NULL &&
                  request("GET", "/api/v1/status", NULL, r, RESP_MAX) &&
                  strstr(r, "\"chip_temperature_c\":") != NULL &&
                  strstr(r, "\"chip_temperature_c\":null") == NULL;
        report(ok, "api_status_chip_temp", "missing or null");
        free(r);
    }
    check("api_history_200", "GET", "/api/v1/history", NULL, 200,
          "\"step_s\"");
    check("api_scan_200", "GET", "/api/v1/wifi/scan", NULL, 200,
          "\"networks\"");

    check("scan_refresh_flag", "GET", "/api/v1/wifi/scan?refresh=1", NULL,
          200, "\"scanning\":true");
    check_cors("cors_api", "/api/v1/config");
    check_cors("cors_current", "/api/v1/current");
    {

        char *resp = malloc(RESP_MAX);
        bool ok = false;
        if (resp != NULL &&
            request("OPTIONS", "/api/v1/config", NULL, resp, RESP_MAX)) {
            char val[64];
            ok = status_of(resp) == 204 &&
                 header_of(resp, "Access-Control-Allow-Origin", val,
                           sizeof val) && strcmp(val, "*") == 0 &&
                 header_of(resp, "Access-Control-Allow-Methods", val,
                           sizeof val) && strstr(val, "POST") != NULL;
        }
        report(ok, "cors_preflight", "OPTIONS /api/v1/config");
        free(resp);
    }

    check("post_bad_json_400", "POST", "/api/v1/config", "not json", 400,
          "error");
    check("post_short_pass_400", "POST", "/api/v1/config",
          "{\"wifi\":{\"ssid\":\"X\",\"password\":\"short\"}}", 400,
          "error");

    char *resp = malloc(RESP_MAX);
    bool have_state = resp != NULL &&
                      request("GET", "/api/v1/config", NULL, resp, RESP_MAX);
    bool renamed = have_state && strstr(resp, "SelfTest-Renamed") != NULL;
    bool provisioning = have_state && strstr(resp, "\"ssid\":\"\"") != NULL;
    (void)provisioning;
    bool mqtt_applied = have_state &&
                        strstr(resp, "\"host\":\"" ST_MQTT_HOST "\"") != NULL;
#ifdef SELFTEST_NTP
    bool ntp_local = have_state &&
                     strstr(resp, "\"ntp_server\":\"127.0.0.1\"") != NULL;
#endif

    if (mqtt_applied) {
        report(strstr(resp, "\"port\":" ST_MQTT_PORT) != NULL,
               "mqtt_port_applied", "saved port missing");
        report(strstr(resp, "\"interval_s\":" ST_MQTT_INTERVAL) != NULL,
               "mqtt_interval_applied", "saved interval missing");
        report(strstr(resp, "\"discovery\":" ST_MQTT_DISC) != NULL,
               "mqtt_discovery_applied", "saved discovery flag missing");
    }
    free(resp);

    if (wifi_mgr_ap_up()) {
        const char *fqdn = renamed ? "selftest-renamed.local" : s_fqdn_default;
        const uint8_t ap_ip[4] = { 0xC0, 0xA8, 0x04, 0x01 };
        report(mdns_ask(fqdn, 1, ap_ip, sizeof ap_ip), "mdns_a", fqdn);
        const uint8_t svc[] = "\x05_http\x04_tcp\x05local";
        report(mdns_ask("_http._tcp.local", 12, svc, sizeof svc - 1),
               "mdns_ptr", "_http._tcp.local");
    } else {
        ESP_LOGW(TAG, "SELFTEST SKIP mdns_a, mdns_ptr (AP down)");
    }

    if (!renamed) {
        check("post_name_ok", "POST", "/api/v1/config",
              "{\"device\":{\"name\":\"SelfTest-Renamed\"}}", 200, "\"ok\"");
        ESP_LOGW(TAG, "SELFTEST: %d passed, %d failed (rename saved, reboot)",
                 s_pass, s_fail);
        vTaskDelete(NULL);
    }
    ESP_LOGW(TAG, "SELFTEST SKIP post_name_ok (already applied)");

#ifdef SELFTEST_POST_WIFI
    if (provisioning) {

        check("post_wifi_wrongpw_200", "POST", "/api/v1/config",
              "{\"wifi\":{\"ssid\":\"SelfTest-Nowifi\","
              "\"password\":\"wrongpassword\"}}", 200, "\"ok\"");
        ESP_LOGW(TAG, "SELFTEST: %d passed, %d failed (wrong wifi saved, "
                 "reboot to connecting)", s_pass, s_fail);
        vTaskDelete(NULL);
    }
    ESP_LOGW(TAG, "SELFTEST SKIP post_wifi_wrongpw_200 (already applied)");
#endif

#ifdef SELFTEST_NTP
    if (!ntp_local) {

        check("post_ntp_ok", "POST", "/api/v1/config",
              "{\"time\":{\"ntp_server\":\"127.0.0.1\"}}", 200, "\"ok\"");
        ESP_LOGW(TAG, "SELFTEST: %d passed, %d failed (loopback NTP saved, "
                 "reboot to check sync)", s_pass, s_fail);
        vTaskDelete(NULL);
    }
    ESP_LOGW(TAG, "SELFTEST SKIP post_ntp_ok (already applied)");

    bool synced = false;
    for (int i = 0; i < 30 && !synced; i++) {
        synced = time_mgr_synced();
        if (!synced) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (!synced) {
        report(false, "sntp_sync", "no sync within 30 s");
    } else {
        int64_t now = time_mgr_unix();
        int64_t want = NTP_TEST_EPOCH + esp_timer_get_time() / 1000000;
        char d[80];
        snprintf(d, sizeof d, "clock %lld, want %lld +/- 3",
                 (long long)now, (long long)want);
        report(now >= want - 3 && now <= want + 3, "sntp_epoch", d);
    }
#endif

    if (!mqtt_applied) {

        check("post_mqtt_ok", "POST", "/api/v1/config",
              "{\"mqtt\":{\"enabled\":true,\"host\":\"" ST_MQTT_HOST "\","
              "\"port\":" ST_MQTT_PORT ",\"interval_s\":" ST_MQTT_INTERVAL
              ",\"discovery\":" ST_MQTT_DISC "}}",
              200, "\"ok\"");
        ESP_LOGW(TAG, "SELFTEST: %d passed, %d failed (mqtt saved, reboot)",
                 s_pass, s_fail);
        vTaskDelete(NULL);
    }
    ESP_LOGW(TAG, "SELFTEST SKIP post_mqtt_ok (already applied)");

#ifdef SELFTEST_MQTT

    mqtt_phase();
#endif

#ifdef SELFTEST_HISTORY

    {
        bool have_hist = false;
        char *h = malloc(RESP_MAX);
        for (int i = 0; i < 45 && h != NULL && !have_hist; i++) {
            if (request("GET", "/api/v1/history", NULL, h, RESP_MAX) &&
                strstr(h, "\"records\":[{") != NULL &&
                strstr(h, "\"t\":179") != NULL) {
                have_hist = true;
            }
            if (!have_hist) {
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }
        free(h);
        report(have_hist, "hist_record", "no record after 90 s");
    }
#endif

#ifdef SELFTEST_WAIT_FALLBACK

    {
        bool fell = false;
        for (int i = 0; i < 240 && !fell; i++) {
            fell = wifi_mgr_ap_up();
            if (!fell) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        report(fell, "fallback_ap_up", "no AP within 240 s");
        if (fell) {
            check("captive_after_fallback", "GET", "/generate_204", NULL, 303,
                  "Location: " CAPTIVE_URL);
            check("api_after_fallback", "GET", "/api/v1/status", NULL, 200,
                  "\"firmware\"");
        }
    }
#endif

#ifdef SELFTEST_FACTORY
    if (renamed) {

        check("post_factory_ok", "POST", "/api/v1/factory-reset", "{}", 200,
              "\"ok\"");
        ESP_LOGW(TAG, "SELFTEST: %d passed, %d failed (factory reset issued)",
                 s_pass, s_fail);
        vTaskDelete(NULL);
    }
    ESP_LOGW(TAG, "SELFTEST SKIP post_factory_ok (already factory)");
#endif

#ifdef SELFTEST_REBOOT

    check("post_reboot_ok", "POST", "/api/v1/reboot", "{}", 200, "\"ok\"");
    ESP_LOGW(TAG, "SELFTEST: %d passed, %d failed (reboot issued)",
             s_pass, s_fail);
    vTaskDelete(NULL);
#endif

    ESP_LOGW(TAG, "SELFTEST: %d passed, %d failed", s_pass, s_fail);
    vTaskDelete(NULL);
}

void selftest_start(void)
{
    build_names();
#ifdef SELFTEST_NTP

    ntp_responder_start();
#endif
#ifdef SELFTEST_MQTT

    if (xTaskCreate(broker_task, "stbroker", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "broker: no task memory");
    }
#endif
    BaseType_t ok = xTaskCreate(selftest_task, "selftest", 6144, NULL, 4, NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

#endif
