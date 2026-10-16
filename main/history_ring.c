/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#include "history_ring.h"

#include <string.h>

void hist_store_init(hist_store_t *s)
{
    memset(s, 0, sizeof *s);
    s->magic = HIST_MAGIC;
    s->version = HIST_VERSION;
}

void hist_store_append(hist_store_t *s, hist_rec_t r)
{
    if (s->count > 0) {
        uint16_t last = (uint16_t)((s->next + HIST_RING_CAP - 1) %
                                   HIST_RING_CAP);
        if (s->rec[last].t == r.t) {
            s->rec[last] = r;
            return;
        }
    }
    s->rec[s->next] = r;
    s->next = (uint16_t)((s->next + 1) % HIST_RING_CAP);
    if (s->count < HIST_RING_CAP) {
        s->count++;
    }
}

uint16_t hist_store_sorted(const hist_store_t *s, hist_rec_t *out)
{
    uint16_t n = 0;

    uint16_t oldest = (uint16_t)((s->next + HIST_RING_CAP - s->count) %
                                 HIST_RING_CAP);
    for (uint16_t i = 0; i < s->count; i++) {
        hist_rec_t r = s->rec[(oldest + i) % HIST_RING_CAP];
        if (r.t == 0) {
            continue;
        }
        int j = (int)n - 1;
        while (j >= 0 && out[j].t > r.t) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = r;
        n++;
    }
    return n;
}
