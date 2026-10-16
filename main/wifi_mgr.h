/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "appconf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MGR_SCAN_MAX 24

typedef enum {
    WIFI_MGR_PROVISIONING,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_NORMAL,
    WIFI_MGR_FALLBACK,
} wifi_mgr_mode_t;

typedef struct {
    char    ssid[APPCONF_SSID_MAX + 1];
    int8_t  rssi;
    bool    secure;
} wifi_mgr_scan_entry_t;

typedef struct {
    wifi_mgr_mode_t mode;
    bool            ap_up;
    bool            sta_connected;
    char            ssid[APPCONF_SSID_MAX + 1];
    char            bssid[18];
    int8_t          rssi;
    uint8_t         channel;
    char            ip[16];
    char            gateway[16];

    char            last_disconnect[16];
} wifi_mgr_status_t;

void wifi_mgr_start(const appconf_t *cfg, const char device_id[5]);

wifi_mgr_mode_t wifi_mgr_mode(void);
const char *wifi_mgr_mode_name(wifi_mgr_mode_t mode);
bool wifi_mgr_ap_up(void);
void wifi_mgr_get_status(wifi_mgr_status_t *out);

void wifi_mgr_scan_start(void);
bool wifi_mgr_scan_busy(void);

size_t wifi_mgr_scan_result(wifi_mgr_scan_entry_t *out, size_t max);

#ifdef __cplusplus
}
#endif
