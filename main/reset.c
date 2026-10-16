/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#include "reset.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "history.h"

static const char *TAG = "reset";

#define FR_MAGIC 0xA5C3F00Du

RTC_NOINIT_ATTR static uint32_t s_fr_magic;
RTC_NOINIT_ATTR static uint32_t s_fr_check;

/* Runs before anything opens NVS: erasing a partition with open handles
 * is unsafe, so the request survives the restart in RTC memory instead. */
void reset_early_init(void)
{
    if (s_fr_magic != FR_MAGIC || s_fr_check != ~FR_MAGIC) {
        return;
    }
    s_fr_magic = 0;
    s_fr_check = 0;
    ESP_LOGW(TAG, "factory reset: erasing NVS before anything opens it");
    ESP_ERROR_CHECK(nvs_flash_erase());
    const esp_partition_t *hist = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, HISTORY_PART_NAME);
    if (hist != NULL) {
        ESP_ERROR_CHECK(esp_partition_erase_range(hist, 0, hist->size));
    }
}

void reset_mark_factory(void)
{
    s_fr_magic = FR_MAGIC;
    s_fr_check = ~FR_MAGIC;
}

void reset_request_factory(void)
{
    reset_mark_factory();
    esp_restart();
}
