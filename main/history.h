/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#pragma once

#include <stdint.h>

#include "history_ring.h"

#define HISTORY_PART_NAME "history"

void history_start(void);

uint16_t history_snapshot(hist_rec_t *out);

uint32_t history_step_s(void);
