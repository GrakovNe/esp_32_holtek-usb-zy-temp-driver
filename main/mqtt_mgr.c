/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"

#include "appconf.h"
#include "fresh.h"
#include "mqtt_mgr.h"
#include "time_mgr.h"
#include "zytemp.h"

static const char *TAG = "mqtt";

#define HA_STATUS_TOPIC "homeassistant/status"

#define AVAIL_ONLINE  "online"
#define AVAIL_OFFLINE "offline"

static struct {
    esp_mqtt_client_handle_t client;
    bool enabled;
    int64_t interval_us;

    char t_co2[APPCONF_TOPIC_MAX + 1];
    char t_temp[APPCONF_TOPIC_MAX + 1];
    char t_avail[48];
    char dev_id[24];
    char unique_co2[40];
    char unique_temp[40];
    char d_co2[80];
    char d_temp[80];

    bool connected;
    bool subscribed;
    bool discovery_on;
    bool discovery_dirty;
    bool cleanup_done;
    bool avail_published;
    bool avail_state;
    int64_t last_pub_mono;
    int64_t last_pub_epoch;
    uint32_t errors;
} ctx;

static char *discovery_json(const char *name, const char *unique_id,
                            const char *state_topic, const char *dev_class,
                            const char *unit)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "unique_id", unique_id);
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "availability_topic", ctx.t_avail);
    cJSON_AddStringToObject(root, "payload_available", AVAIL_ONLINE);
    cJSON_AddStringToObject(root, "payload_not_available", AVAIL_OFFLINE);
    cJSON_AddStringToObject(root, "device_class", dev_class);
    cJSON_AddStringToObject(root, "unit_of_measurement", unit);
    cJSON_AddStringToObject(root, "state_class", "measurement");

    cJSON *dev = cJSON_AddObjectToObject(root, "device");
    if (dev != NULL) {
        cJSON_AddStringToObject(dev, "identifiers", ctx.dev_id);
        cJSON_AddStringToObject(dev, "name", "Air Monitor");
        cJSON_AddStringToObject(dev, "manufacturer", "Holtek");
        cJSON_AddStringToObject(dev, "model", "USB-zyTemp on ESP32-S3");
        cJSON_AddStringToObject(dev, "sw_version", APP_VER);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static void publish_discovery(void)
{
    char *cfg_co2 = discovery_json("CO2", ctx.unique_co2, ctx.t_co2,
                                   "carbon_dioxide", "ppm");
    char *cfg_temp = discovery_json("Temperature", ctx.unique_temp, ctx.t_temp,
                                    "temperature", "°C");
    if (cfg_co2 == NULL || cfg_temp == NULL) {
        ctx.errors++;
    } else {
        int r1 = esp_mqtt_client_publish(ctx.client, ctx.d_co2, cfg_co2, 0, 1,
                                         true);
        int r2 = esp_mqtt_client_publish(ctx.client, ctx.d_temp, cfg_temp, 0,
                                         1, true);
        if (r1 < 0 || r2 < 0) {
            ctx.errors++;
            ESP_LOGW(TAG, "discovery publish failed");
        } else {
            ESP_LOGI(TAG, "discovery configs published");
        }
    }
    free(cfg_co2);
    free(cfg_temp);
}

static void publish_discovery_cleanup(void)
{
    esp_mqtt_client_publish(ctx.client, ctx.d_co2, "", 0, 1, true);
    esp_mqtt_client_publish(ctx.client, ctx.d_temp, "", 0, 1, true);
    ctx.cleanup_done = true;
}

static void publish_availability(bool on)
{
    int r = esp_mqtt_client_publish(ctx.client, ctx.t_avail,
                                    on ? AVAIL_ONLINE : AVAIL_OFFLINE, 0, 1,
                                    true);
    if (r < 0) {
        ctx.errors++;
        return;
    }
    ctx.avail_published = true;
    ctx.avail_state = on;
    ESP_LOGI(TAG, "availability: %s", on ? AVAIL_ONLINE : AVAIL_OFFLINE);
}

static void publish_readings(int64_t now_epoch)
{
    zytemp_reading_t r;
    zytemp_get_reading(&r);

    bool any = false;
    if (co2_fresh(&r)) {
        char val[16];
        snprintf(val, sizeof val, "%d", r.co2_ppm);
        if (esp_mqtt_client_publish(ctx.client, ctx.t_co2, val, 0, 1,
                                    false) >= 0) {
            any = true;
        }
    }
    if (temp_fresh(&r)) {
        char val[16];
        snprintf(val, sizeof val, "%.2f", r.temp_c);
        if (esp_mqtt_client_publish(ctx.client, ctx.t_temp, val, 0, 1,
                                    false) >= 0) {
            any = true;
        }
    }
    if (!any) {

        return;
    }
    ctx.last_pub_mono = esp_timer_get_time();
    ctx.last_pub_epoch = now_epoch;
    ESP_LOGI(TAG, "published co2=%s temp=%s", co2_fresh(&r) ? "y" : "-",
             temp_fresh(&r) ? "y" : "-");
}

static void subscribe_ha_status(void)
{
    if (esp_mqtt_client_subscribe(ctx.client, HA_STATUS_TOPIC, 0) < 0) {
        ctx.errors++;
        ESP_LOGW(TAG, "subscribe %s failed", HA_STATUS_TOPIC);
        return;
    }
    ctx.subscribed = true;
    ESP_LOGI(TAG, "subscribed to %s", HA_STATUS_TOPIC);
}

/* Every publish and subscribe happens here. esp-mqtt callbacks must not
 * block, and esp_mqtt_client_* calls from its own event task deadlock it. */
static void publish_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (ctx.client == NULL) {
            continue;
        }

        if (!ctx.connected) {
            continue;
        }

        zytemp_reading_t r;
        zytemp_get_reading(&r);
        bool on = co2_fresh(&r) || temp_fresh(&r);
        if (!ctx.avail_published || on != ctx.avail_state) {
            publish_availability(on);
        }

        if (ctx.discovery_on && !ctx.subscribed) {
            subscribe_ha_status();
        }
        if (ctx.discovery_dirty) {
            if (ctx.discovery_on) {
                publish_discovery();
            } else if (!ctx.cleanup_done) {
                publish_discovery_cleanup();
            }
            ctx.discovery_dirty = false;
        }

        int64_t now = time_mgr_unix();
        if (now <= 0) {
            continue; /* no readings with a wrong timestamp */
        }
        if (ctx.last_pub_mono != 0 &&
            esp_timer_get_time() - ctx.last_pub_mono < ctx.interval_us) {
            continue;
        }
        publish_readings(now);
    }
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id,
                       void *data)
{
    esp_mqtt_event_t *ev = data;

    switch (id) {
    case MQTT_EVENT_CONNECTED:

        ctx.avail_published = false;
        ctx.discovery_dirty = true;
        ctx.subscribed = false;
        ctx.connected = true;
        ESP_LOGI(TAG, "connected to broker");
        break;

    case MQTT_EVENT_DISCONNECTED:
        ctx.connected = false;
        ESP_LOGW(TAG, "disconnected from broker");
        break;

    case MQTT_EVENT_DATA:
        /* Home Assistant publishes "online" here when it restarts and has
         * forgotten the discovered entities. */
        if (ev->topic_len == (int)strlen(HA_STATUS_TOPIC) &&
            strncmp(ev->topic, HA_STATUS_TOPIC, ev->topic_len) == 0 &&
            ev->data_len == (int)strlen(AVAIL_ONLINE) &&
            strncmp(ev->data, AVAIL_ONLINE, ev->data_len) == 0) {
            ESP_LOGI(TAG, "home assistant online, republishing discovery");
            ctx.discovery_dirty = true;
        }
        break;

    case MQTT_EVENT_ERROR:
        ctx.errors++;
        ESP_LOGW(TAG, "mqtt error (total %" PRIu32 ")", ctx.errors);
        break;

    default:
        break;
    }
}

esp_err_t mqtt_mgr_start(const appconf_t *cfg, const char *device_id)
{
    if (!cfg->mqtt_enabled) {
        ESP_LOGI(TAG, "mqtt disabled");
        return ESP_OK;
    }
    ctx.enabled = true;
    ctx.discovery_on = cfg->mqtt_discovery;
    ctx.interval_us = (int64_t)cfg->mqtt_interval_s * 1000000;

    snprintf(ctx.t_co2, sizeof ctx.t_co2, "%s", cfg->mqtt_co2_topic);
    snprintf(ctx.t_temp, sizeof ctx.t_temp, "%s", cfg->mqtt_temp_topic);
    snprintf(ctx.t_avail, sizeof ctx.t_avail, "airmonitor/%s/availability",
             device_id);
    snprintf(ctx.dev_id, sizeof ctx.dev_id, "airmonitor_%s", device_id);
    snprintf(ctx.unique_co2, sizeof ctx.unique_co2, "%s_co2", ctx.dev_id);
    snprintf(ctx.unique_temp, sizeof ctx.unique_temp, "%s_temperature",
             ctx.dev_id);
    snprintf(ctx.d_co2, sizeof ctx.d_co2, "homeassistant/sensor/%s/config",
             ctx.unique_co2);
    snprintf(ctx.d_temp, sizeof ctx.d_temp, "homeassistant/sensor/%s/config",
             ctx.unique_temp);

    esp_mqtt_client_config_t mc = { 0 };
    char uri[APPCONF_HOST_MAX + 32];
    bool ipv6 = strchr(cfg->mqtt_host, ':') != NULL;
    snprintf(uri, sizeof uri, "%s://%s%s%s:%u", cfg->mqtt_tls ? "mqtts" : "mqtt",
             ipv6 ? "[" : "", cfg->mqtt_host, ipv6 ? "]" : "",
             (unsigned)cfg->mqtt_port);
    mc.broker.address.uri = uri;
    if (cfg->mqtt_username[0] != '\0') {
        mc.credentials.username = cfg->mqtt_username;
    }
    if (cfg->mqtt_password[0] != '\0') {
        mc.credentials.authentication.password = cfg->mqtt_password;
    }

    if (cfg->mqtt_tls) {
        if (cfg->mqtt_skip_verify) {

            ESP_LOGW(TAG, "TLS certificate verification is DISABLED");
        } else if (cfg->mqtt_ca_cert[0] != '\0') {
            mc.broker.verification.certificate = cfg->mqtt_ca_cert;
        } else {
            mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        }
    }

    mc.session.last_will.topic = ctx.t_avail;
    mc.session.last_will.msg = AVAIL_OFFLINE;
    mc.session.last_will.msg_len = 0;
    mc.session.last_will.qos = 1;
    mc.session.last_will.retain = true;
    mc.session.keepalive = 30;
    mc.network.reconnect_timeout_ms = 5000;
    mc.network.timeout_ms = 10000;

    ctx.client = esp_mqtt_client_init(&mc);
    if (ctx.client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed (out of memory?)");
        ctx.enabled = false;
        return ESP_ERR_NO_MEM;
    }
    esp_mqtt_client_register_event(ctx.client, ESP_EVENT_ANY_ID, mqtt_event,
                                   NULL);
    esp_err_t err = esp_mqtt_client_start(ctx.client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(publish_task, "mqtt_pub", 6144, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "cannot create publish task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "mqtt started: %s interval %u s discovery %s", uri,
             (unsigned)cfg->mqtt_interval_s,
             cfg->mqtt_discovery ? "on" : "off");
    return ESP_OK;
}

void mqtt_mgr_get_status(mqtt_mgr_status_t *out)
{
    out->enabled = ctx.enabled;
    out->connected = ctx.connected;
    out->last_publish = ctx.last_pub_epoch;
    out->errors = ctx.errors;
}
