/*
 * RNG200 tests (WS2.5).
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/rng.h>
#include <bm/test.h>

#define DRAW_WORDS              256     /* 1 KiB */
#define RNG_WAIT_US             1000000

static uint32_t ones(uint32_t word)
{
    return __builtin_popcount(word);
}

/*
 * Start the generator as Linux does and draw 1 KiB. The warm-up bits it
 * was asked to discard count, and the data passes crude sanity checks: no
 * word repeats the one before, and the bits are balanced to within nine
 * standard deviations (45 bits for 8192).
 */
TEST(rng_draw, "rng/draw")
{
    static uint32_t buf[DRAW_WORDS];
    bool was_running = rng_running();
    uint32_t bits = 0;
    unsigned got;

    rng_start();
    got = rng_words(buf, DRAW_WORDS, RNG_WAIT_US);
    if (!was_running) {
        rng_stop();
    }
    ASSERT_EQ(got, DRAW_WORDS);
    if (!was_running) {
        ASSERT_GE(rng_read(RNG_TOTAL_BIT_COUNT),
                  RNG_WARMUP_BITS + DRAW_WORDS * 32);
    }

    for (unsigned i = 0; i < DRAW_WORDS; i++) {
        bits += ones(buf[i]);
        if (i) {
            ASSERT_MSG(buf[i] != buf[i - 1], "word %u repeats: 0x%x", i,
                       buf[i]);
        }
    }
    bm_test_note("rng: %u of %u bits set", bits, DRAW_WORDS * 32);
    ASSERT_GE(bits, DRAW_WORDS * 16 - 400);
    ASSERT_LE(bits, DRAW_WORDS * 16 + 400);
}

/*
 * Linux's recovery sequence (iproc_rng200_restart()): while held in soft
 * reset the FIFO is empty; once released the generator refills it.
 */
TEST(rng_soft_reset, "rng/soft-reset")
{
    bool was_running = rng_running();
    uint32_t word, ctrl;

    rng_start();
    ctrl = rng_read(RNG_CTRL);
    ASSERT_EQ(rng_words(&word, 1, RNG_WAIT_US), 1);

    rng_write(RNG_CTRL, ctrl & ~RNG_CTRL_RBGEN_MASK);
    rng_write(RNG_INT_STATUS, 0xffffffff);
    rng_write(RBG_SOFT_RESET, 1);
    rng_write(RNG_SOFT_RESET, 1);
    ASSERT_EQ(rng_read(RNG_FIFO_COUNT) & RNG_FIFO_COUNT_MASK, 0);
    rng_write(RNG_SOFT_RESET, 0);
    rng_write(RBG_SOFT_RESET, 0);
    rng_write(RNG_CTRL, ctrl);

    ASSERT_EQ(rng_words(&word, 1, RNG_WAIT_US), 1);
    ASSERT_EQ(rng_read(RNG_INT_STATUS) &
              (RNG_INT_STATUS_NIST_FAIL | RNG_INT_STATUS_MASTER_FAIL_LOCKOUT),
              0);
    if (!was_running) {
        rng_stop();
    }
}
