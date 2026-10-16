/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#pragma once

#include <stdint.h>

#define HIST_RING_CAP 144
#define HIST_MAGIC    0x48537431u
#define HIST_VERSION  1

#define HIST_NO_TEMP ((int16_t)-32768)

typedef struct {
    uint32_t t;
    uint16_t co2_ppm;
    int16_t  temp_c100;
} hist_rec_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t next;
    uint16_t count;
    uint16_t pad;
    hist_rec_t rec[HIST_RING_CAP];
} hist_store_t;

void hist_store_init(hist_store_t *s);

void hist_store_append(hist_store_t *s, hist_rec_t r);

uint16_t hist_store_sorted(const hist_store_t *s, hist_rec_t *out);
