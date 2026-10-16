/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "appconf.h"

void time_mgr_start(const appconf_t *cfg);

bool time_mgr_synced(void);

int64_t time_mgr_unix(void);
