/*
 * Power management block: the watchdog and the reset status.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/io.h>
#include <bm/pm.h>
#include <bm/runtime.h>

uint32_t pm_read(uint32_t reg)
{
    return mmio_read32(bm_plat.pm + reg);
}

void pm_write(uint32_t reg, uint32_t val)
{
    mmio_write32(bm_plat.pm + reg, PM_PASSWORD | val);
}

void pm_watchdog_start(uint32_t ticks)
{
    uint32_t rstc;

    pm_write(PM_WDOG, ticks & PM_WDOG_TIME_MASK);
    rstc = pm_read(PM_RSTC) & ~(PM_PASSWORD_MASK | PM_RSTC_WRCFG_MASK);
    pm_write(PM_RSTC, rstc | PM_RSTC_WRCFG_FULL_RESET);
}

void pm_watchdog_stop(void)
{
    pm_write(PM_RSTC, PM_RSTC_RESET);
}

uint32_t pm_watchdog_left(void)
{
    return pm_read(PM_WDOG) & PM_WDOG_TIME_MASK;
}

/* The partition number is spread over the even bits 0-10 */
unsigned pm_partition(void)
{
    uint32_t rsts = pm_read(PM_RSTS);
    unsigned partition = 0;

    for (unsigned i = 0; i < 6; i++) {
        partition |= ((rsts >> (2 * i)) & 1) << i;
    }
    return partition;
}

void pm_set_partition(unsigned partition)
{
    uint32_t rsts = pm_read(PM_RSTS) & ~PM_RSTS_PARTITION_MASK;

    for (unsigned i = 0; i < 6; i++) {
        rsts |= ((partition >> i) & 1) << (2 * i);
    }
    pm_write(PM_RSTS, rsts & ~PM_PASSWORD_MASK);
}
