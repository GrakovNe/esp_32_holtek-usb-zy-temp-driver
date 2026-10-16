/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#include "boot_btn.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "reset.h"

static const char *TAG = "boot-btn";

#define BOOT_GPIO GPIO_NUM_0
#define POLL_MS   50
#define HOLD_MS   5000

static void boot_btn_task(void *arg)
{
    (void)arg;
    uint32_t held_ms = 0;
    bool armed = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if (gpio_get_level(BOOT_GPIO) == 0) {
            held_ms += POLL_MS;
            /* GPIO0 is a strapping pin: restarting while it is held boots
             * into download mode. Reset only after the release. */
            if (!armed && held_ms >= HOLD_MS) {
                armed = true;
                ESP_LOGW(TAG, "BOOT held 5 s: release it to factory-reset now");
            }
        } else {
            if (armed) {
                ESP_LOGW(TAG, "BOOT released: factory reset, restarting");
                reset_request_factory();
            }
            held_ms = 0;
        }
    }
}

void boot_btn_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    BaseType_t ok = xTaskCreate(boot_btn_task, "boot_btn", 3072, NULL, 3, NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
