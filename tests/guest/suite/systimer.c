/*
 * VideoCore system timer tests (WS2.1).
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/exception.h>
#include <bm/gic.h>
#include <bm/runtime.h>
#include <bm/test.h>
#include <bm/timer.h>

/* BCM2835 ARM Peripherals, chapter 12 */
#define ST_CS                   0x00
#define ST_CLO                  0x04
#define ST_CHI                  0x08
#define ST_C(n)                 (0x0c + 4 * (n))

static uint32_t st_read(uint32_t reg)
{
    return mmio_read32(bm_plat.systimer + reg);
}

static void st_write(uint32_t reg, uint32_t val)
{
    mmio_write32(bm_plat.systimer + reg, val);
}

/*
 * The 1 MHz counter agrees with the generic timer. Each system timer read
 * is bracketed by generic counter reads, so the time it reports must lie
 * between the inner and outer brackets however slow MMIO accesses are.
 *
 * Deferred (PLAN.md P22): QEMU derives the generic counter from a whole
 * number of nanoseconds per tick, 18 for 54 MHz, so it runs 2.9% fast
 * (55.6 MHz) against every other clock. Allow 4% until that is fixed;
 * the note reports the measured rate.
 */
TEST(systimer_rate, "systimer/rate")
{
    const uint64_t tolerance_pct = 4;
    uint64_t a = counter_now();
    uint32_t st0 = st_read(ST_CLO);
    uint64_t b = counter_now();

    delay_us(2000);

    uint64_t c = counter_now();
    uint32_t st1 = st_read(ST_CLO);
    uint64_t d = counter_now();
    uint64_t st_us = st1 - st0;
    uint64_t freq = counter_freq();

    /* In ticks: st_us * freq / 1e6 against the brackets */
    uint64_t st_ticks = st_us * freq / 1000000;
    uint64_t one_us = freq / 1000000;

    bm_test_note("systimer/rate: %lu us of system timer = %lu-%lu ticks "
                 "at a nominal %lu Hz", st_us, c - b, d - a, freq);
    ASSERT_GE((st_ticks + one_us) * (100 + tolerance_pct), (c - b) * 100);
    ASSERT_LE(st_ticks * (100 - tolerance_pct), (d - a + one_us) * 100);
}

static volatile unsigned st_fired, st_intid;
static volatile uint32_t st_cs, st_clo;

/* Record the match and acknowledge it, which drops the interrupt */
static void st_handler(unsigned intid, void *arg)
{
    unsigned n = (uintptr_t)arg;

    st_intid = intid;
    st_cs = st_read(ST_CS);
    st_clo = st_read(ST_CLO);
    st_write(ST_CS, BIT(n));
    st_fired++;
}

TEST(systimer_compare, "systimer/compare")
{
    for (unsigned n = 0; n < BM_SYSTIMER_COMPARATORS; n++) {
        unsigned intid = bm_plat.systimer_intid[n];
        uint32_t match, armed;
        bool fired;

        st_write(ST_CS, 0xf);
        st_fired = 0;
        irq_register(intid, st_handler, (void *)(uintptr_t)n);
        gic_enable(intid);
        irq_unmask();

        /*
         * The comparator matches when the counter's low 32 bits equal it,
         * so a vCPU that a busy host deschedules between reading the
         * counter and writing the comparator can arm it a wrap of the
         * counter away: arm it again if the counter had reached the match
         * by the time it was written.
         */
        for (unsigned tries = 0; tries < 3; tries++) {
            match = st_read(ST_CLO) + 300;
            st_write(ST_C(n), match);
            armed = st_read(ST_CLO);
            ASSERT_EQ(st_read(ST_C(n)), match);
            fired = wait_until(st_fired, 100000);
            if (fired || (int32_t)(armed - match) < 0) {
                break;                  /* fired, or armed in time */
            }
        }

        irq_mask();
        gic_disable(intid);
        irq_unregister(intid);
        ASSERT_MSG(fired, "comparator %u did not interrupt", n);
        ASSERT_EQ(st_intid, intid);
        ASSERT_EQ(st_cs, BIT(n));
        /* Not before the match; how late depends on the host under TCG */
        ASSERT_MSG((int32_t)(st_clo - match) >= 0,
                   "comparator %u fired at 0x%x, before 0x%x", n, st_clo,
                   match);
        ASSERT_EQ(st_fired, 1);
        /* Acknowledged: status clear and the level interrupt gone */
        ASSERT_EQ(st_read(ST_CS), 0);
        ASSERT(!gic_is_pending(intid));
    }
}
