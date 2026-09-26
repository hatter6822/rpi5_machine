/*
 * The RNG200 random number generator.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/io.h>
#include <bm/rng.h>
#include <bm/runtime.h>
#include <bm/timer.h>

uint32_t rng_read(uint32_t reg)
{
    return mmio_read32(bm_plat.rng + reg);
}

void rng_write(uint32_t reg, uint32_t val)
{
    mmio_write32(bm_plat.rng + reg, val);
}

bool rng_running(void)
{
    return rng_read(RNG_CTRL) & RNG_CTRL_RBGEN_MASK;
}

void rng_start(void)
{
    if (rng_running()) {
        return;
    }
    rng_write(RNG_TOTAL_BIT_COUNT_THRESHOLD, RNG_WARMUP_BITS);
    rng_write(RNG_FIFO_COUNT, 2 << RNG_FIFO_THRESHOLD_SHIFT);
    /* 1 MHz sample rate */
    rng_write(RNG_CTRL, (3 << RNG_CTRL_DIV_CTRL_SHIFT) | RNG_CTRL_RBGEN_MASK);
}

void rng_stop(void)
{
    rng_write(RNG_CTRL, rng_read(RNG_CTRL) & ~RNG_CTRL_RBGEN_MASK);
}

unsigned rng_words(uint32_t *out, unsigned n, uint64_t wait_us)
{
    uint64_t deadline = timeout_us(wait_us);
    unsigned done = 0;

    while (done < n) {
        uint32_t avail = 0;

        if (rng_read(RNG_TOTAL_BIT_COUNT) > 16) {
            avail = rng_read(RNG_FIFO_COUNT) & RNG_FIFO_COUNT_MASK;
        }
        if (!avail) {
            if (timeout_expired(deadline)) {
                break;
            }
            cpu_relax();
            continue;
        }
        while (avail-- && done < n) {
            out[done++] = rng_read(RNG_FIFO_DATA);
        }
    }
    return done;
}
