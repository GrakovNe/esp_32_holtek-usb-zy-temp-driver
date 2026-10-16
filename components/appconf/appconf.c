/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#include "appconf.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool str_fits(const char *s, size_t cap)
{
    return s != NULL && strnlen(s, cap + 1) <= cap;
}

static bool has_ctrl(const char *s)
{
    for (const char *p = s; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) {
            return true;
        }
    }
    return false;
}

static bool is_printable_ascii(const char *s)
{
    for (const char *p = s; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

static bool host_chars_ok(const char *s)
{
    for (const char *p = s; *p != '\0'; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool topic_ok(const char *s)
{
    if (s[0] == '\0') {
        return false;
    }
    for (const char *p = s; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x21 || c > 0x7e || c == '+' || c == '#') {
            return false;
        }
    }
    return true;
}

void appconf_device_id(const uint8_t mac[6], char out[5])
{
    snprintf(out, 5, "%02x%02x", mac[4], mac[5]);
}

void appconf_hostname(const char *device_name, const char device_id[5],
                      char *out, size_t out_sz)
{
    if (out_sz == 0) {
        return;
    }
    size_t o = 0;
    bool pending_dash = false;
    for (const char *p = device_name; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        char keep = 0;
        if (c >= 'A' && c <= 'Z') {
            keep = (char)(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            keep = (char)c;
        }
        if (keep != 0) {

            if (pending_dash && o > 0 && o + 2 <= out_sz) {
                out[o++] = '-';
            }
            if (o + 2 <= out_sz) {
                out[o++] = keep;
            }
            pending_dash = false;
        } else {
            pending_dash = true;
        }
    }
    while (o > 0 && out[o - 1] == '-') {
        o--;
    }
    out[o] = '\0';
    if (o == 0) {
        snprintf(out, out_sz, "airmonitor-%s", device_id);
    }
}

void appconf_default_name(const char device_id[5], char *out, size_t out_sz)
{
    char id_upper[5] = { 0 };
    for (int i = 0; i < 4; i++) {
        id_upper[i] = (char)toupper((unsigned char)device_id[i]);
    }
    snprintf(out, out_sz, "AirMonitor-%s", id_upper);
}

void appconf_defaults(appconf_t *c, const char device_id[5])
{
    memset(c, 0, sizeof *c);
    appconf_default_name(device_id, c->device_name, sizeof c->device_name);
    snprintf(c->ntp_server, sizeof c->ntp_server, "%s", APPCONF_NTP_DEFAULT);
    c->mqtt_port = APPCONF_MQTT_PORT_PLAIN;
    c->mqtt_interval_s = APPCONF_MQTT_INTERVAL_DEFAULT_S;
    snprintf(c->mqtt_co2_topic, sizeof c->mqtt_co2_topic,
             "airmonitor/%s/co2", device_id);
    snprintf(c->mqtt_temp_topic, sizeof c->mqtt_temp_topic,
             "airmonitor/%s/temperature", device_id);
    appconf_derive(c, device_id);
}

void appconf_derive(appconf_t *c, const char device_id[5])
{
    appconf_hostname(c->device_name, device_id, c->hostname, sizeof c->hostname);
}

bool appconf_validate(const appconf_t *c, char *err, size_t err_sz)
{
#define BAD(...)                       \
    do {                               \
        snprintf(err, err_sz, __VA_ARGS__); \
        return false;                  \
    } while (0)

    if (!str_fits(c->device_name, APPCONF_NAME_MAX)) {
        BAD("Device name is too long (max %d).", APPCONF_NAME_MAX);
    }
    if (c->device_name[0] == '\0') {
        BAD("Device name is empty.");
    }
    if (has_ctrl(c->device_name)) {
        BAD("Device name has control characters.");
    }

    if (!str_fits(c->wifi_ssid, APPCONF_SSID_MAX)) {
        BAD("Wi-Fi SSID is too long (max %d).", APPCONF_SSID_MAX);
    }
    if (has_ctrl(c->wifi_ssid)) {
        BAD("Wi-Fi SSID has control characters.");
    }
    if (!str_fits(c->wifi_password, APPCONF_PASS_MAX)) {
        BAD("Wi-Fi password is too long (max %d).", APPCONF_PASS_MAX);
    }
    if (!is_printable_ascii(c->wifi_password)) {
        BAD("Wi-Fi password must be printable ASCII.");
    }
    if (c->wifi_ssid[0] == '\0' && c->wifi_password[0] != '\0') {
        BAD("Wi-Fi password without an SSID.");
    }

    size_t pass_len = strlen(c->wifi_password);
    if (c->wifi_ssid[0] != '\0' && pass_len > 0 && pass_len < 8) {
        BAD("Wi-Fi password must be at least 8 characters.");
    }

    if (!str_fits(c->ntp_server, APPCONF_HOST_MAX)) {
        BAD("NTP server is too long (max %d).", APPCONF_HOST_MAX);
    }
    if (c->ntp_server[0] == '\0') {
        BAD("NTP server is empty.");
    }
    if (!host_chars_ok(c->ntp_server)) {
        BAD("NTP server has invalid characters.");
    }

    if (c->mqtt_enabled) {
        if (!str_fits(c->mqtt_host, APPCONF_HOST_MAX)) {
            BAD("MQTT host is too long (max %d).", APPCONF_HOST_MAX);
        }
        if (c->mqtt_host[0] == '\0') {
            BAD("MQTT host is empty.");
        }
        if (!host_chars_ok(c->mqtt_host)) {
            BAD("MQTT host has invalid characters.");
        }
        if (c->mqtt_port == 0) {
            BAD("MQTT port must be 1-65535.");
        }
        if (!str_fits(c->mqtt_username, APPCONF_USER_MAX)) {
            BAD("MQTT user is too long (max %d).", APPCONF_USER_MAX);
        }
        if (!is_printable_ascii(c->mqtt_username)) {
            BAD("MQTT user must be printable ASCII.");
        }
        if (!str_fits(c->mqtt_password, APPCONF_PASS_MAX)) {
            BAD("MQTT password is too long (max %d).", APPCONF_PASS_MAX);
        }
        if (!is_printable_ascii(c->mqtt_password)) {
            BAD("MQTT password must be printable ASCII.");
        }
        if (!str_fits(c->mqtt_co2_topic, APPCONF_TOPIC_MAX) ||
            !topic_ok(c->mqtt_co2_topic)) {
            BAD("CO2 topic is empty or invalid (max %d, no spaces or + #).",
                APPCONF_TOPIC_MAX);
        }
        if (!str_fits(c->mqtt_temp_topic, APPCONF_TOPIC_MAX) ||
            !topic_ok(c->mqtt_temp_topic)) {
            BAD("Temperature topic is empty or invalid (max %d, no spaces or + #).",
                APPCONF_TOPIC_MAX);
        }
        if (c->mqtt_interval_s < APPCONF_MQTT_INTERVAL_MIN_S ||
            c->mqtt_interval_s > APPCONF_MQTT_INTERVAL_MAX_S) {
            BAD("MQTT interval must be %d-%d s.",
                APPCONF_MQTT_INTERVAL_MIN_S, APPCONF_MQTT_INTERVAL_MAX_S);
        }
    }

    if (!str_fits(c->mqtt_ca_cert, APPCONF_CERT_MAX)) {
        BAD("CA certificate is too long (max %d).", APPCONF_CERT_MAX);
    }
    if (c->mqtt_ca_cert[0] != '\0' &&
        strstr(c->mqtt_ca_cert, "-----BEGIN") == NULL) {
        BAD("CA certificate does not look like PEM.");
    }

    return true;
#undef BAD
}
