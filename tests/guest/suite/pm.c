/*
 * Power management and watchdog tests (WS2.4).
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/io.h>
#include <bm/pm.h>
#include <bm/runtime.h>
#include <bm/test.h>
#include <bm/timer.h>

#define ST_CLO                  0x04    /* system timer, 1 MHz */

static uint32_t st_now(void)
{
    return mmio_read32(bm_plat.systimer + ST_CLO);
}

/*
 * The countdown runs at 65536 Hz while armed and holds while disarmed.
 * Time is taken from the system timer, which runs off the same clock in
 * QEMU (the generic counter does not quite, see systimer/rate), with
 * reads bracketing the countdown: at least 10 ms and at most the outer
 * bracket passed.
 */
TEST(pm_watchdog_countdown, "pm/watchdog-countdown")
{
    const uint32_t ticks = PM_WDOG_TICKS_PER_SEC;       /* 1 s */
    uint32_t before, start, after, left, held;
    uint64_t min, max;

    before = st_now();
    pm_watchdog_start(ticks);
    start = st_now();
    wait_until(st_now() - start >= 10000, 100000);
    left = pm_watchdog_left();
    after = st_now();
    pm_watchdog_stop();

    min = 10000ull * PM_WDOG_TICKS_PER_SEC / 1000000;
    max = ((after - before + 1) * (uint64_t)PM_WDOG_TICKS_PER_SEC +
           999999) / 1000000;
    ASSERT_GE(ticks - left, min);
    ASSERT_LE(ticks - left, max);

    held = pm_watchdog_left();
    delay_us(10000);
    ASSERT_EQ(pm_watchdog_left(), held);
    ASSERT_EQ(pm_read(PM_RSTC) & PM_RSTC_WRCFG_MASK, 0);
}

/*
 * Arm a 10 tick (150 us) watchdog, as Linux does to reboot, and check
 * after the reset that the boot count went up and the reset status says
 * the watchdog did it, keeping the partition set beforehand.
 */
TEST(pm_watchdog_reset, "pm/watchdog-reset")
{
    uint64_t *boot = &bm_test_scratch()[0];
    unsigned partition;
    uint32_t rsts;

    if (bm_test_resets() == 0) {
        *boot = bm_boot_count();
        if (*boot == 1) {
            rsts = pm_read(PM_RSTS);
            ASSERT_EQ(rsts & (PM_RSTS_HADWRF | PM_RSTS_HADPOR),
                      PM_RSTS_HADPOR);
        }
        pm_set_partition(42);
        pm_watchdog_start(10);
        wait_until(false, 100000);
        ASSERT_MSG(false, "no reset 100 ms after arming the watchdog");
    }

    rsts = pm_read(PM_RSTS);
    partition = pm_partition();
    pm_set_partition(0);
    ASSERT_EQ(bm_test_resets(), 1);
    ASSERT_EQ(bm_boot_count(), *boot + 1);
    ASSERT_EQ(rsts & (PM_RSTS_HADWRF | PM_RSTS_HADPOR), PM_RSTS_HADWRF);
    ASSERT_EQ(partition, 42);
    ASSERT_EQ(pm_read(PM_RSTC), PM_RSTC_RESET);
    ASSERT_EQ(pm_watchdog_left(), 0);
}
