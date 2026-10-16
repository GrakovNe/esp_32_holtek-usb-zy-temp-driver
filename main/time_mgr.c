/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#include "time_mgr.h"

#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

static const char *TAG = "time";

static volatile bool s_synced;

static void on_sync(struct timeval *tv)
{

    s_synced = true;
    ESP_LOGI(TAG, "synced, unix time %lld", (long long)tv->tv_sec);
}

void time_mgr_start(const appconf_t *cfg)
{
    if (cfg->ntp_server[0] == '\0') {
        ESP_LOGW(TAG, "no NTP server configured, time stays unsynced");
        return;
    }

    /* esp_netif_sntp keeps the server string by pointer; cfg is static. */
    esp_sntp_config_t scfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(cfg->ntp_server);
    scfg.sync_cb = on_sync;
    scfg.wait_for_sync = false;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&scfg));
    ESP_LOGI(TAG, "sntp started, server \"%s\"", cfg->ntp_server);
}

bool time_mgr_synced(void)
{
    return s_synced;
}

int64_t time_mgr_unix(void)
{
    if (!s_synced) {
        return -1;
    }
    return (int64_t)time(NULL);
}
