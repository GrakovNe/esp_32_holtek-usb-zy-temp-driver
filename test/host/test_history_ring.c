/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#include "history_ring.h"

#include "tinytest.h"

static hist_rec_t mk(uint32_t t, uint16_t co2, int16_t temp)
{
    hist_rec_t r = { .t = t, .co2_ppm = co2, .temp_c100 = temp };
    return r;
}

static void test_init_empty(void)
{
    hist_store_t s;
    hist_store_init(&s);
    CHECK(s.magic == HIST_MAGIC);
    CHECK(s.version == HIST_VERSION);
    CHECK(s.count == 0);

    hist_rec_t out[HIST_RING_CAP];
    CHECK(hist_store_sorted(&s, out) == 0);
}

static void test_append_read(void)
{
    hist_store_t s;
    hist_store_init(&s);
    hist_store_append(&s, mk(100, 600, 2391));
    hist_store_append(&s, mk(200, 0, HIST_NO_TEMP));
    hist_store_append(&s, mk(300, 610, -50));

    hist_rec_t out[HIST_RING_CAP];
    CHECK(hist_store_sorted(&s, out) == 3);
    CHECK(out[0].t == 100 && out[0].co2_ppm == 600 &&
          out[0].temp_c100 == 2391);
    CHECK(out[1].t == 200 && out[1].co2_ppm == 0 &&
          out[1].temp_c100 == HIST_NO_TEMP);
    CHECK(out[2].t == 300 && out[2].co2_ppm == 610 &&
          out[2].temp_c100 == -50);
}

static void test_replace_same_window(void)
{
    hist_store_t s;
    hist_store_init(&s);
    hist_store_append(&s, mk(100, 600, 2391));
    hist_store_append(&s, mk(200, 601, 2392));

    hist_store_append(&s, mk(200, 605, 2395));

    CHECK(s.count == 2);
    hist_rec_t out[HIST_RING_CAP];
    CHECK(hist_store_sorted(&s, out) == 2);
    CHECK(out[1].t == 200 && out[1].co2_ppm == 605 &&
          out[1].temp_c100 == 2395);

    hist_store_append(&s, mk(150, 599, 2390));
    CHECK(s.count == 3);
    CHECK(hist_store_sorted(&s, out) == 3);
    CHECK(out[1].t == 150);
}

static void test_wrap(void)
{
    hist_store_t s;
    hist_store_init(&s);

    for (uint32_t i = 0; i < HIST_RING_CAP + 5; i++) {
        hist_store_append(&s, mk(1000 + i * 10, (uint16_t)(500 + i), 2000));
    }
    CHECK(s.count == HIST_RING_CAP);

    hist_rec_t out[HIST_RING_CAP];
    CHECK(hist_store_sorted(&s, out) == HIST_RING_CAP);
    CHECK(out[0].t == 1000 + 5 * 10);
    CHECK(out[HIST_RING_CAP - 1].t ==
          1000 + (HIST_RING_CAP + 4) * 10);
    for (uint16_t i = 1; i < HIST_RING_CAP; i++) {
        CHECK(out[i].t == out[i - 1].t + 10);
    }
}

static void test_backwards_clock(void)
{
    hist_store_t s;
    hist_store_init(&s);
    hist_store_append(&s, mk(300, 600, 2390));
    hist_store_append(&s, mk(200, 601, 2391));
    hist_store_append(&s, mk(400, 602, 2392));

    hist_rec_t out[HIST_RING_CAP];
    CHECK(hist_store_sorted(&s, out) == 3);
    CHECK(out[0].t == 200 && out[1].t == 300 && out[2].t == 400);
}

static void test_empty_slot_skipped(void)
{
    hist_store_t s;
    hist_store_init(&s);

    s.rec[0] = mk(100, 600, 2390);
    s.rec[2] = mk(300, 602, 2392);
    s.rec[3] = mk(400, 603, 2393);
    s.next = 4;
    s.count = 4;

    hist_rec_t out[HIST_RING_CAP];
    CHECK(hist_store_sorted(&s, out) == 3);
    CHECK(out[0].t == 100 && out[1].t == 300 && out[2].t == 400);
}

int main(void)
{
    test_init_empty();
    test_append_read();
    test_replace_same_window();
    test_wrap();
    test_backwards_clock();
    test_empty_slot_skipped();
    REPORT();
}
