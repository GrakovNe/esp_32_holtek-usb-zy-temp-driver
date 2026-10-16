/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#pragma once

#include <stdio.h>
#include <string.h>

static int tt_checks = 0;
static int tt_fails = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        tt_checks++;                                                     \
        if (!(cond)) {                                                   \
            tt_fails++;                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                \
    } while (0)

#define CHECK_STR(actual, expected)                                      \
    do {                                                                 \
        tt_checks++;                                                     \
        if (strcmp((actual), (expected)) != 0) {                         \
            tt_fails++;                                                  \
            printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, \
                   (actual), (expected));                                \
        }                                                                \
    } while (0)

#define REPORT()                                                    \
    do {                                                            \
        printf("%d checks, %d failed\n", tt_checks, tt_fails);      \
        return tt_fails != 0 ? 1 : 0;                               \
    } while (0)
