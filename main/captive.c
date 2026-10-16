/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#include "captive.h"

#include <errno.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "captive";

#define DNS_PORT        53
#define RECV_TIMEOUT_MS 500
#define DNS_QTYPE_A     1

static SemaphoreHandle_t s_stopped;
static volatile bool s_running;

#define DNS_BUF_SIZE 512

static int dns_build_reply(uint8_t *buf, size_t len, uint32_t ap_ip_nbo)
{
    if (len < 12) {
        return 0;
    }

    size_t i = 12;
    while (i < len && buf[i] != 0) {
        uint8_t label = buf[i];
        if ((label & 0xC0) == 0xC0) {
            return 0;
        }
        i += 1 + label;
    }
    if (i >= len || i + 5 > len) {
        return 0;
    }
    uint16_t qtype = (uint16_t)((buf[i + 1] << 8) | buf[i + 2]);
    size_t out = i + 5;
    if (out + 16 > DNS_BUF_SIZE) {
        return 0;
    }

    buf[2] = 0x81;
    buf[3] = 0x80;
    buf[6] = 0; buf[7] = 0;
    buf[8] = 0; buf[9] = 0;
    buf[10] = 0; buf[11] = 0;
    if (qtype != DNS_QTYPE_A) {
        return (int)out; /* NOERROR, no answers: an A record here breaks AAAA clients */
    }
    buf[7] = 1;

    buf[out++] = 0xC0; buf[out++] = 0x0C;
    buf[out++] = 0x00; buf[out++] = 0x01;
    buf[out++] = 0x00; buf[out++] = 0x01;
    buf[out++] = 0x00; buf[out++] = 0x00;
    buf[out++] = 0x00; buf[out++] = 0x3C;
    buf[out++] = 0x00; buf[out++] = 0x04;
    memcpy(buf + out, &ap_ip_nbo, 4);
    out += 4;
    return (int)out;
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        goto done;
    }
    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct timeval tv = { .tv_sec = RECV_TIMEOUT_MS / 1000,
                          .tv_usec = (RECV_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        ESP_LOGE(TAG, "bind :53: errno %d", errno);
        close(sock);
        goto done;
    }
    ESP_LOGI(TAG, "DNS on :53");

    uint32_t ap_ip_nbo = 0;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"),
                              &ip) == ESP_OK) {
        ap_ip_nbo = ip.ip.addr;
    }

    uint8_t buf[DNS_BUF_SIZE];
    while (s_running) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof src;
        int len = recvfrom(sock, buf, sizeof buf, 0,
                           (struct sockaddr *)&src, &src_len);
        if (len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                ESP_LOGW(TAG, "recvfrom: errno %d", errno);
            }
            continue;
        }
        int out = dns_build_reply(buf, (size_t)len, ap_ip_nbo);
        if (out > 0) {
            sendto(sock, buf, out, 0, (struct sockaddr *)&src, src_len);
        }
    }
    close(sock);

done:
    xSemaphoreGive(s_stopped);
    vTaskDelete(NULL);
}

void captive_start(void)
{
    if (s_running) {
        return;
    }
    s_running = true;
    if (s_stopped == NULL) {
        s_stopped = xSemaphoreCreateBinary();
        configASSERT(s_stopped != NULL);
    }
    xSemaphoreTake(s_stopped, 0);
    BaseType_t ok = xTaskCreate(dns_task, "capdns", 3072, NULL, 6, NULL);
    configASSERT(ok == pdPASS);
}

void captive_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;

    if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(3 * RECV_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "DNS task did not stop in time");
    }
}
