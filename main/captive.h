/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#pragma once

#define CAPTIVE_AP_IP  "192.168.4.1"
#define CAPTIVE_URL    "http://" CAPTIVE_AP_IP "/"

#ifdef __cplusplus
extern "C" {
#endif

void captive_start(void);

void captive_stop(void);

#ifdef __cplusplus
}
#endif
