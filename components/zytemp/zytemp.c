/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

#include "zytemp.h"
#include "zytemp_protocol.h"

static const char *TAG = "zytemp";

#define DEFAULT_TASK_PRIORITY   5
#define TASK_STACK_SIZE         4096
#define SESSION_QUEUE_LEN       4
#define RESTART_BACKOFF_MIN_MS  100
#define RESTART_BACKOFF_MAX_MS  5000
#define DEFAULT_STALL_MS        30000
#define WATCHDOG_PERIOD_MS      1000
#define PORT_OFF_TIME_MS        1000
#define DEFAULT_CO2_WARMUP_MS   30000

typedef enum {
    MSG_ATTACH,
    MSG_RESTART,
} msg_kind_t;

typedef struct {
    msg_kind_t               kind;
    hid_host_device_handle_t hid;
} session_msg_t;

static struct {
    bool                     started;
    zytemp_config_t          cfg;
    SemaphoreHandle_t        lock;
    QueueHandle_t            queue;
    hid_host_device_handle_t hid;
    zytemp_reading_t         r;
    TickType_t               co2_tick;
    TickType_t               temp_tick;
    TickType_t               frame_tick;
    TickType_t               connect_tick;
    uint32_t                 warmup_ms;
    uint32_t                 stall_ms;
    uint32_t                 restart_streak;
} ctx;

static void emit(const zytemp_event_t *ev)
{
    if (ctx.cfg.callback) {
        ctx.cfg.callback(ev, ctx.cfg.callback_arg);
    }
}

static void post(msg_kind_t kind, hid_host_device_handle_t hid)
{
    const session_msg_t msg = { .kind = kind, .hid = hid };
    if (xQueueSend(ctx.queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "session queue full, event %d dropped", kind);
    }
}

static uint32_t age_ms(TickType_t since)
{
    return (uint32_t)((xTaskGetTickCount() - since) * portTICK_PERIOD_MS);
}

static bool co2_warming_up(void)
{
    return ctx.r.connected && age_ms(ctx.connect_tick) < ctx.warmup_ms;
}

static void on_input_report(hid_host_device_handle_t hid)
{
    uint8_t raw[ZYTEMP_FRAME_LEN] = { 0 };
    size_t len = 0;
    esp_err_t err = hid_host_device_get_raw_input_report_data(hid, raw, sizeof raw, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get_raw_input_report_data: %s", esp_err_to_name(err));
        return;
    }

    uint8_t item;
    uint16_t value;
    bool ok = len == ZYTEMP_FRAME_LEN && zytemp_parse_frame(raw, &item, &value);

    ESP_LOGD(TAG, "frame (%u bytes, %s): %02x %02x %02x %02x %02x %02x %02x %02x",
             (unsigned)len, ok ? "ok" : "bad",
             raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);

    zytemp_event_t ev = { 0 };
    bool report = false;
    TickType_t now = xTaskGetTickCount();
    xSemaphoreTake(ctx.lock, portMAX_DELAY);
    ctx.frame_tick = now;
    ctx.restart_streak = 0;
    if (!ok) {
        ctx.r.frames_bad++;
    } else {
        ctx.r.frames_valid++;
        if (item == ZYTEMP_ITEM_CO2 && co2_warming_up()) {
            ESP_LOGD(TAG, "CO2 %" PRIu16 " ppm dropped (sensor warming up)", value);
        } else if (item == ZYTEMP_ITEM_CO2) {
            ctx.r.co2_valid = true;
            ctx.r.co2_ppm = value;
            ctx.co2_tick = now;
            ev = (zytemp_event_t){ .type = ZYTEMP_EVENT_CO2, .co2_ppm = value };
            report = true;
        } else if (item == ZYTEMP_ITEM_TEMP) {
            ctx.r.temp_valid = true;
            ctx.r.temp_c = zytemp_temp_c(value);
            ctx.temp_tick = now;
            ev = (zytemp_event_t){ .type = ZYTEMP_EVENT_TEMPERATURE, .temp_c = ctx.r.temp_c };
            report = true;
        }

    }
    xSemaphoreGive(ctx.lock);

    if (report) {
        emit(&ev);
    }
}

static void on_disconnected(hid_host_device_handle_t hid)
{
    xSemaphoreTake(ctx.lock, portMAX_DELAY);
    bool was_ours = ctx.hid == hid;
    bool was_streaming = was_ours && ctx.r.connected;
    if (was_ours) {
        ctx.hid = NULL;
        ctx.r.connected = false;
        ctx.r.co2_valid = false;
        ctx.r.temp_valid = false;
    }
    xSemaphoreGive(ctx.lock);

    hid_host_device_close(hid);

    if (was_streaming) {
        ESP_LOGW(TAG, "sensor disconnected");
        const zytemp_event_t ev = { .type = ZYTEMP_EVENT_DISCONNECTED };
        emit(&ev);
    }
}

static void hid_interface_event_cb(hid_host_device_handle_t hid,
                                   const hid_host_interface_event_t event,
                                   void *arg)
{
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        on_input_report(hid);
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        /* The driver does not resubmit a failed IN transfer and leaves the
         * interface ACTIVE, so the pipe has to be stopped and started. */
        ESP_LOGW(TAG, "input transfer error, restarting input pipe");
        post(MSG_RESTART, hid);
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        on_disconnected(hid);
        break;
    default:
        ESP_LOGD(TAG, "interface event %d", event);
    }
}

static void hid_driver_event_cb(hid_host_device_handle_t hid,
                                const hid_host_driver_event_t event,
                                void *arg)
{
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        post(MSG_ATTACH, hid);
    }
}

static bool is_zytemp(hid_host_device_handle_t hid)
{
    hid_host_dev_info_t info;
    if (hid_host_get_device_info(hid, &info) != ESP_OK) {
        return false;
    }
    ESP_LOGD(TAG, "HID device %04" PRIx16 ":%04" PRIx16, info.VID, info.PID);
    return info.VID == ZYTEMP_VID && info.PID == ZYTEMP_PID;
}

static void send_start_key(hid_host_device_handle_t hid)
{
    uint8_t key[ZYTEMP_FRAME_LEN];
    memcpy(key, ZYTEMP_START_KEY, sizeof key);
    esp_err_t err = hid_class_request_set_report(hid, HID_REPORT_TYPE_FEATURE,
                                                 0, key, sizeof key);
    if (err != ESP_OK) {
        /* Newer sensor firmware streams without the start key. */
        ESP_LOGW(TAG, "start key rejected (%s), streaming anyway", esp_err_to_name(err));
    }
}

static void attach(hid_host_device_handle_t hid)
{

    if (!is_zytemp(hid)) {
        return;
    }

    /* Claimed before open, so an unplug during attach() is noticed. */
    xSemaphoreTake(ctx.lock, portMAX_DELAY);
    bool busy = ctx.hid != NULL;
    if (!busy) {
        ctx.hid = hid;
    }
    xSemaphoreGive(ctx.lock);
    if (busy) {
        ESP_LOGW(TAG, "another zyTemp is already in use, ignoring this one");
        return;
    }

    const hid_host_device_config_t dev_cfg = {
        .callback = hid_interface_event_cb,
        .callback_arg = NULL,
    };
    esp_err_t err = hid_host_device_open(hid, &dev_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device_open: %s", esp_err_to_name(err));
        xSemaphoreTake(ctx.lock, portMAX_DELAY);
        ctx.hid = NULL;
        xSemaphoreGive(ctx.lock);
        return;
    }

    /* The sensor sends no input reports until the host has read its
     * report descriptor; without this request it NAKs every IN poll. */
    size_t rd_len = 0;
    if (hid_host_get_report_descriptor(hid, &rd_len) == NULL) {
        ESP_LOGW(TAG, "report descriptor request failed");
    }

    send_start_key(hid);

    err = hid_host_device_start(hid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device_start: %s", esp_err_to_name(err));

        hid_host_device_close(hid);
        return;
    }

    xSemaphoreTake(ctx.lock, portMAX_DELAY);
    bool still_ours = ctx.hid == hid;
    ctx.r.connected = still_ours;
    ctx.frame_tick = xTaskGetTickCount();
    ctx.connect_tick = ctx.frame_tick;
    ctx.restart_streak = 0;
    xSemaphoreGive(ctx.lock);
    if (!still_ours) {
        return;
    }

    ESP_LOGI(TAG, "sensor connected, streaming");
    const zytemp_event_t ev = { .type = ZYTEMP_EVENT_CONNECTED };
    emit(&ev);
}

static void restart_input(hid_host_device_handle_t hid)
{

    xSemaphoreTake(ctx.lock, portMAX_DELAY);
    uint32_t streak = ctx.restart_streak++;
    xSemaphoreGive(ctx.lock);
    uint32_t delay_ms = RESTART_BACKOFF_MIN_MS << (streak < 6 ? streak : 6);
    if (delay_ms > RESTART_BACKOFF_MAX_MS) {
        delay_ms = RESTART_BACKOFF_MAX_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(delay_ms));

    xSemaphoreTake(ctx.lock, portMAX_DELAY);
    bool current = ctx.hid == hid;
    xSemaphoreGive(ctx.lock);
    if (!current) {
        return;
    }

    esp_err_t err = hid_host_device_stop(hid);
    if (err == ESP_OK) {
        err = hid_host_device_start(hid);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "input pipe restart failed: %s", esp_err_to_name(err));
    }
}

/* A connected sensor that stays silent is re-plugged in software: the
 * root port power cycle makes the host stack drop and re-enumerate it. */
static void check_stall(void)
{
    if (ctx.cfg.usb_host_installed_externally) {
        return;
    }
    xSemaphoreTake(ctx.lock, portMAX_DELAY);
    bool stalled = ctx.r.connected && age_ms(ctx.frame_tick) > ctx.stall_ms;
    xSemaphoreGive(ctx.lock);
    if (!stalled) {
        return;
    }

    ESP_LOGW(TAG, "no data for %" PRIu32 " ms, re-plugging the sensor (root port power cycle)",
             ctx.stall_ms);
    esp_err_t err = usb_host_lib_set_root_port_power(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "root port power off: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(PORT_OFF_TIME_MS));
    err = usb_host_lib_set_root_port_power(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "root port power on: %s", esp_err_to_name(err));
    }
}

/* All blocking control transfers run here. Their completion event is
 * delivered by the hid-host task, so issuing one from a hid-host callback
 * waits on itself until the 5 s timeout. */
static void session_task(void *arg)
{
    session_msg_t msg;
    for (;;) {
        if (xQueueReceive(ctx.queue, &msg, pdMS_TO_TICKS(WATCHDOG_PERIOD_MS)) != pdTRUE) {
            check_stall();
            continue;
        }
        switch (msg.kind) {
        case MSG_ATTACH:
            attach(msg.hid);
            break;
        case MSG_RESTART:
            restart_input(msg.hid);
            break;
        }
    }
}

static void usb_lib_task(void *arg)
{
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

esp_err_t zytemp_start(const zytemp_config_t *config)
{
    ESP_RETURN_ON_FALSE(!ctx.started, ESP_ERR_INVALID_STATE, TAG, "already started");

    if (config) {
        ctx.cfg = *config;
    }
    ctx.warmup_ms = ctx.cfg.co2_warmup_ms ? ctx.cfg.co2_warmup_ms
                                          : DEFAULT_CO2_WARMUP_MS;
    ctx.stall_ms = ctx.cfg.stall_timeout_ms ? ctx.cfg.stall_timeout_ms
                                            : DEFAULT_STALL_MS;
    const int prio = ctx.cfg.task_priority > 1 ? ctx.cfg.task_priority
                                                : DEFAULT_TASK_PRIORITY;

    ctx.lock = xSemaphoreCreateMutex();
    ctx.queue = xQueueCreate(SESSION_QUEUE_LEN, sizeof(session_msg_t));
    ESP_RETURN_ON_FALSE(ctx.lock && ctx.queue, ESP_ERR_NO_MEM, TAG, "no memory");

    if (!ctx.cfg.usb_host_installed_externally) {
        const usb_host_config_t host_cfg = {
            .skip_phy_setup = false,
            .intr_flags = ESP_INTR_FLAG_LEVEL1,
        };
        ESP_RETURN_ON_ERROR(usb_host_install(&host_cfg), TAG, "usb_host_install");
        ESP_RETURN_ON_FALSE(xTaskCreate(usb_lib_task, "usb_lib", TASK_STACK_SIZE,
                                        NULL, prio + 1, NULL) == pdPASS,
                            ESP_ERR_NO_MEM, TAG, "usb_lib task");
    }

    ESP_RETURN_ON_FALSE(xTaskCreate(session_task, "zytemp", TASK_STACK_SIZE,
                                    NULL, prio - 1, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "session task");

    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = prio,
        .stack_size = TASK_STACK_SIZE,
        .core_id = tskNO_AFFINITY,
        .callback = hid_driver_event_cb,
        .callback_arg = NULL,
    };
    ESP_RETURN_ON_ERROR(hid_host_install(&hid_cfg), TAG, "hid_host_install");

    ctx.started = true;
    ESP_LOGI(TAG, "USB host ready, waiting for the sensor");
    return ESP_OK;
}

esp_err_t zytemp_get_reading(zytemp_reading_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(ctx.started, ESP_ERR_INVALID_STATE, TAG, "not started");

    xSemaphoreTake(ctx.lock, portMAX_DELAY);
    *out = ctx.r;
    out->co2_warming_up = co2_warming_up();
    out->co2_age_ms  = ctx.r.co2_valid  ? age_ms(ctx.co2_tick)  : 0;
    out->temp_age_ms = ctx.r.temp_valid ? age_ms(ctx.temp_tick) : 0;
    xSemaphoreGive(ctx.lock);
    return ESP_OK;
}
