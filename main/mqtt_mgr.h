/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "appconf.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mqtt_mgr_start(const appconf_t *cfg, const char *device_id);

typedef struct {
    bool enabled;
    bool connected;
    int64_t last_publish;
    uint32_t errors;
} mqtt_mgr_status_t;

void mqtt_mgr_get_status(mqtt_mgr_status_t *out);

#ifdef __cplusplus
}
#endif
