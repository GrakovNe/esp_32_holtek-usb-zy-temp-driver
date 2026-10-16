/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#include "zytemp_protocol.h"

#include <stddef.h>

_Static_assert(ZYTEMP_FRAME_LEN == 8, "de-scramble transform requires 8-byte frames");

enum { F_ITEM = 0, F_HI = 1, F_LO = 2, F_CKSUM = 3, F_TRAILER = 4 };

const uint8_t ZYTEMP_START_KEY[ZYTEMP_FRAME_LEN] = {
    0xc4, 0xc6, 0xc0, 0x92, 0x40, 0x23, 0xdc, 0x96
};

static void descramble(const uint8_t in[ZYTEMP_FRAME_LEN],
                       uint8_t out[ZYTEMP_FRAME_LEN])
{
    enum { N = ZYTEMP_FRAME_LEN };
    static const uint8_t cstate[N] = { 0x48, 0x74, 0x65, 0x6d, 0x70, 0x39, 0x39, 0x65 };
    static const int shuffle[N] = { 2, 4, 0, 7, 1, 6, 5, 3 };
    uint8_t p1[N], p2[N], p3[N], ctmp[N];

    for (int i = 0; i < N; i++) {
        p1[shuffle[i]] = in[i];
    }
    for (int i = 0; i < N; i++) {
        p2[i] = p1[i] ^ ZYTEMP_START_KEY[i];
    }
    for (int i = 0; i < N; i++) {
        p3[i] = (uint8_t)((p2[i] >> 3) | (p2[(i - 1 + N) % N] << 5));
    }
    for (int i = 0; i < N; i++) {
        ctmp[i] = (uint8_t)((cstate[i] >> 4) | (cstate[i] << 4));
    }
    for (int i = 0; i < N; i++) {
        out[i] = (uint8_t)(0x100 + p3[i] - ctmp[i]);
    }
}

static bool frame_valid(const uint8_t f[ZYTEMP_FRAME_LEN])
{
    return f[F_TRAILER] == ZYTEMP_FRAME_TRAILER &&
           (uint8_t)(f[F_ITEM] + f[F_HI] + f[F_LO]) == f[F_CKSUM];
}

bool zytemp_parse_frame(const uint8_t raw[ZYTEMP_FRAME_LEN],
                        uint8_t *item, uint16_t *value)
{
    uint8_t dec[ZYTEMP_FRAME_LEN];
    descramble(raw, dec);

    /* Older sensors scramble frames, newer ones send them plain. */
    const uint8_t *f = frame_valid(dec) ? dec
                     : frame_valid(raw) ? raw
                     : NULL;
    if (f == NULL) {
        return false;
    }

    *item = f[F_ITEM];
    *value = (uint16_t)((f[F_HI] << 8) | f[F_LO]);
    return true;
}

float zytemp_temp_c(uint16_t value)
{
    return value / 16.0f - 273.15f;
}
