/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "fresh.h"
#include "history.h"
#include "time_mgr.h"
#include "zytemp.h"

static const char *TAG = "history";

#define HIST_NS  "hist"
#define HIST_KEY "ring"

#ifndef HIST_WINDOW_S
#define HIST_WINDOW_S 600u
#endif
#ifndef HIST_SAMPLE_MS
#define HIST_SAMPLE_MS 5000u
#endif

static hist_store_t s_store;
static SemaphoreHandle_t s_lock;
static nvs_handle_t s_nvs;

static uint32_t s_window;
static uint32_t s_co2_sum;
static uint16_t s_co2_n;
static int32_t s_temp_sum;
static uint16_t s_temp_n;

static void store_save_locked(void)
{
    esp_err_t err = nvs_set_blob(s_nvs, HIST_KEY, &s_store, sizeof s_store);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit: %s", esp_err_to_name(err));
    }
}

static void close_window_locked(void)
{
    if (s_co2_n == 0 && s_temp_n == 0) {
        return;
    }
    hist_rec_t r = { .t = s_window };
    if (s_co2_n > 0) {
        r.co2_ppm = (uint16_t)lroundf((float)s_co2_sum / (float)s_co2_n);
    }
    if (s_temp_n > 0) {
        r.temp_c100 = (int16_t)lroundf((float)s_temp_sum / (float)s_temp_n);
    } else {
        r.temp_c100 = HIST_NO_TEMP;
    }
    hist_store_append(&s_store, r);
    store_save_locked();

    char co2s[8] = "-";
    if (r.co2_ppm != 0) {
        snprintf(co2s, sizeof co2s, "%" PRIu16, r.co2_ppm);
    }
    char ts[12] = "-";
    if (r.temp_c100 != HIST_NO_TEMP) {
        snprintf(ts, sizeof ts, "%.2f", r.temp_c100 / 100.0);
    }
    ESP_LOGI(TAG, "window %" PRIu32 ": co2 %s ppm, temp %s C (n %u/%u)",
             r.t, co2s, ts, s_co2_n, s_temp_n);
}

static void sample_once(void)
{
    zytemp_reading_t rd = { 0 };
    zytemp_get_reading(&rd);

    int64_t now = time_mgr_unix();
    if (now < 0) {
        return; /* no records before the clock is synced */
    }
    uint32_t window = (uint32_t)(now - now % HIST_WINDOW_S);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_window == 0) {
        s_window = window;
    } else if (window != s_window) {

        if (window > s_window) {
            close_window_locked();
        }
        s_window = window;
        s_co2_sum = s_co2_n = 0;
        s_temp_sum = s_temp_n = 0;
    }

    if (co2_fresh(&rd) && !rd.co2_warming_up) {
        s_co2_sum += rd.co2_ppm;
        s_co2_n++;
    }
    if (temp_fresh(&rd)) {
        s_temp_sum += (int32_t)lroundf(rd.temp_c * 100.0f);
        s_temp_n++;
    }
    xSemaphoreGive(s_lock);
}

static void history_task(void *arg)
{
    (void)arg;
    while (true) {
        sample_once();
        vTaskDelay(pdMS_TO_TICKS(HIST_SAMPLE_MS));
    }
}

void history_start(void)
{
    esp_err_t err = nvs_open_from_partition(HISTORY_PART_NAME, HIST_NS,
                                            NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s, history disabled", esp_err_to_name(err));
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "no memory, history disabled");
        return;
    }

    size_t sz = sizeof s_store;
    err = nvs_get_blob(s_nvs, HIST_KEY, &s_store, &sz);
    if (err == ESP_OK && sz == sizeof s_store &&
        s_store.magic == HIST_MAGIC && s_store.version == HIST_VERSION) {

        if (s_store.count > HIST_RING_CAP) {
            s_store.count = HIST_RING_CAP;
        }
        if (s_store.next >= HIST_RING_CAP) {
            s_store.next = 0;
        }
        ESP_LOGI(TAG, "ring loaded: %u records", s_store.count);
    } else {

        hist_store_init(&s_store);
        store_save_locked();
        ESP_LOGI(TAG, "ring created");
    }

    if (xTaskCreate(history_task, "history", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the sampling task, history frozen");
    }
}

uint16_t history_snapshot(hist_rec_t *out)
{
    if (s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint16_t n = hist_store_sorted(&s_store, out);
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t history_step_s(void)
{
    return HIST_WINDOW_S;
}
