/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "appconf.h"
#include "boot_btn.h"
#include "history.h"
#include "mdns.h"
#include "mqtt_mgr.h"
#include "reset.h"
#include "sdkconfig.h"
#include "settings.h"
#include "time_mgr.h"
#include "web_server.h"
#include "wifi_mgr.h"
#include "zytemp.h"

#if CONFIG_APP_SELFTEST
#include "selftest.h"
#endif

static const char *TAG = "main";

#ifndef APP_VER
#define APP_VER "dev"
#endif

static const uint32_t SUMMARY_PERIOD_MS = 5000;

static void on_sensor_event(const zytemp_event_t *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case ZYTEMP_EVENT_CO2:
        ESP_LOGI(TAG, "CO2: %" PRIu16 " ppm", ev->co2_ppm);
        break;
    case ZYTEMP_EVENT_TEMPERATURE:
        ESP_LOGI(TAG, "Temperature: %.2f C", ev->temp_c);
        break;
    case ZYTEMP_EVENT_CONNECTED:
        ESP_LOGI(TAG, "sensor connected");
        break;
    case ZYTEMP_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "sensor disconnected");
        break;
    }
}

static appconf_t s_cfg;

static void mdns_start(const appconf_t *cfg)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init: %s", esp_err_to_name(err));
        return;
    }
    err = mdns_hostname_set(cfg->hostname);
    if (err == ESP_OK) {
        err = mdns_instance_name_set(cfg->device_name);
    }
    if (err == ESP_OK) {
        err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns registration: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "mdns: http://%s.local/", cfg->hostname);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "AirMonitor firmware %s (idf %s, built %s %s)",
             APP_VER, esp_get_idf_version(), __DATE__, __TIME__);

    reset_early_init();
    settings_nvs_init();

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    char device_id[5];
    appconf_device_id(mac, device_id);

    appconf_t *cfg = &s_cfg;
    settings_load(cfg, device_id);
    ESP_LOGI(TAG, "device %s: name \"%s\", hostname %s.local",
             device_id, cfg->device_name, cfg->hostname);
    ESP_LOGI(TAG, "wifi: %s, mqtt: %s, ntp: %s",
             cfg->wifi_ssid[0] != '\0' ? cfg->wifi_ssid : "not configured",
             cfg->mqtt_enabled ? cfg->mqtt_host : "disabled",
             cfg->ntp_server);

    boot_btn_start();
    wifi_mgr_start(cfg, device_id);
    web_server_start(cfg);
    time_mgr_start(cfg);
    mdns_start(cfg);

    const zytemp_config_t zcfg = { .callback = on_sensor_event };
    ESP_ERROR_CHECK(zytemp_start(&zcfg));
    history_start();
    mqtt_mgr_start(cfg, device_id);

#if CONFIG_APP_SELFTEST
    selftest_start();
#endif

    zytemp_reading_t r;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SUMMARY_PERIOD_MS));
        zytemp_get_reading(&r);
        if (!r.connected) {
            ESP_LOGI(TAG, "waiting for sensor...");
            continue;
        }
        char co2[32] = "n/a", temp[32] = "n/a";
        if (r.co2_warming_up) {
            snprintf(co2, sizeof co2, "warming up");
        } else if (r.co2_valid) {
            snprintf(co2, sizeof co2, "%" PRIu16 " ppm (%" PRIu32 " s ago)",
                     r.co2_ppm, r.co2_age_ms / 1000);
        }
        if (r.temp_valid) {
            snprintf(temp, sizeof temp, "%.2f C (%" PRIu32 " s ago)",
                     r.temp_c, r.temp_age_ms / 1000);
        }
        ESP_LOGI(TAG, "summary: CO2 %s, T %s, frames %" PRIu32 " ok / %" PRIu32 " bad",
                 co2, temp, r.frames_valid, r.frames_bad);
    }
}
