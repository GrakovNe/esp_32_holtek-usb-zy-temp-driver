/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APPCONF_NAME_MAX  32
#define APPCONF_SSID_MAX  32
#define APPCONF_PASS_MAX  64
#define APPCONF_HOST_MAX  64
#define APPCONF_USER_MAX  32
#define APPCONF_TOPIC_MAX 96
#define APPCONF_CERT_MAX  3072

#define APPCONF_NTP_DEFAULT             "pool.ntp.org"
#define APPCONF_MQTT_PORT_PLAIN         1883
#define APPCONF_MQTT_PORT_TLS           8883
#define APPCONF_MQTT_INTERVAL_DEFAULT_S 30
#define APPCONF_MQTT_INTERVAL_MIN_S     5
#define APPCONF_MQTT_INTERVAL_MAX_S     3600

typedef struct {
    char     device_name[APPCONF_NAME_MAX + 1];
    char     wifi_ssid[APPCONF_SSID_MAX + 1];
    char     wifi_password[APPCONF_PASS_MAX + 1];
    char     ntp_server[APPCONF_HOST_MAX + 1];
    bool     mqtt_enabled;
    char     mqtt_host[APPCONF_HOST_MAX + 1];
    uint16_t mqtt_port;
    char     mqtt_username[APPCONF_USER_MAX + 1];
    char     mqtt_password[APPCONF_PASS_MAX + 1];
    bool     mqtt_tls;
    char     mqtt_ca_cert[APPCONF_CERT_MAX + 1];
    bool     mqtt_skip_verify;
    char     mqtt_co2_topic[APPCONF_TOPIC_MAX + 1];
    char     mqtt_temp_topic[APPCONF_TOPIC_MAX + 1];
    uint16_t mqtt_interval_s;
    bool     mqtt_discovery;

    char     hostname[APPCONF_HOST_MAX + 1];
} appconf_t;

void appconf_device_id(const uint8_t mac[6], char out[5]);

void appconf_default_name(const char device_id[5], char *out, size_t out_sz);

void appconf_defaults(appconf_t *c, const char device_id[5]);

void appconf_hostname(const char *device_name, const char device_id[5],
                      char *out, size_t out_sz);

void appconf_derive(appconf_t *c, const char device_id[5]);

bool appconf_validate(const appconf_t *c, char *err, size_t err_sz);

#ifdef __cplusplus
}
#endif
