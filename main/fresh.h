/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#pragma once

#include <stdbool.h>

#include "zytemp.h"

/* The sensor sends a burst every ~2.5 s; 30 s of silence means it stalled. */
#define FRESH_MAX_AGE_MS 30000u

static inline bool co2_fresh(const zytemp_reading_t *r)
{
    return r->co2_valid && r->co2_age_ms <= FRESH_MAX_AGE_MS;
}

static inline bool temp_fresh(const zytemp_reading_t *r)
{
    return r->temp_valid && r->temp_age_ms <= FRESH_MAX_AGE_MS;
}
