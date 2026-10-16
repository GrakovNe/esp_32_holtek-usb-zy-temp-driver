/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZYTEMP_VID              0x04d9
#define ZYTEMP_PID              0xa052

#define ZYTEMP_FRAME_LEN        8
#define ZYTEMP_FRAME_TRAILER    0x0d
#define ZYTEMP_ITEM_CO2         0x50
#define ZYTEMP_ITEM_TEMP        0x42

extern const uint8_t ZYTEMP_START_KEY[ZYTEMP_FRAME_LEN];

bool zytemp_parse_frame(const uint8_t raw[ZYTEMP_FRAME_LEN],
                        uint8_t *item, uint16_t *value);

float zytemp_temp_c(uint16_t value);

#ifdef __cplusplus
}
#endif
