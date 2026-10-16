/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     connected;
    bool     co2_warming_up;
    bool     co2_valid;
    uint16_t co2_ppm;
    uint32_t co2_age_ms;
    bool     temp_valid;
    float    temp_c;
    uint32_t temp_age_ms;
    uint32_t frames_valid;
    uint32_t frames_bad;
} zytemp_reading_t;

typedef enum {
    ZYTEMP_EVENT_CONNECTED,
    ZYTEMP_EVENT_DISCONNECTED,
    ZYTEMP_EVENT_CO2,
    ZYTEMP_EVENT_TEMPERATURE,
} zytemp_event_type_t;

typedef struct {
    zytemp_event_type_t type;
    union {
        uint16_t co2_ppm;
        float    temp_c;
    };
} zytemp_event_t;

typedef void (*zytemp_event_cb_t)(const zytemp_event_t *event, void *arg);

typedef struct {
    zytemp_event_cb_t callback;
    void             *callback_arg;

    bool              usb_host_installed_externally;
    int               task_priority;

    uint32_t          stall_timeout_ms;

    uint32_t          co2_warmup_ms;
} zytemp_config_t;

esp_err_t zytemp_start(const zytemp_config_t *config);

esp_err_t zytemp_get_reading(zytemp_reading_t *out);

#ifdef __cplusplus
}
#endif
