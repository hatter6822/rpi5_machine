/*
 * Power management block: the watchdog and the reset status.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Registers as used by Linux drivers/watchdog/bcm2835_wdt.c. Every write
 * carries a password in bits 31:24.
 */

#ifndef BM_PM_H
#define BM_PM_H

#include <stdint.h>

#define PM_RSTC                 0x1c
#define PM_RSTC_WRCFG_MASK      0x30
#define PM_RSTC_WRCFG_FULL_RESET 0x20
#define PM_RSTC_RESET           0x102
#define PM_RSTS                 0x20
#define PM_RSTS_PARTITION_MASK  0x555
#define PM_RSTS_HADWRF          0x20    /* the watchdog reset the chip */
#define PM_RSTS_HADPOR          0x1000  /* power-on reset */
#define PM_WDOG                 0x24
#define PM_WDOG_TIME_MASK       0xfffff
#define PM_WDOG_TICKS_PER_SEC   65536
#define PM_PASSWORD             0x5a000000
#define PM_PASSWORD_MASK        0xff000000

uint32_t pm_read(uint32_t reg);
void pm_write(uint32_t reg, uint32_t val);

/* Arm the watchdog to reset the chip after @ticks (bcm2835_wdt_start()) */
void pm_watchdog_start(uint32_t ticks);
void pm_watchdog_stop(void);

/* Ticks left before the watchdog fires */
uint32_t pm_watchdog_left(void);

/* The boot partition that RSTS carries across a reset, 0-63 */
unsigned pm_partition(void);
void pm_set_partition(unsigned partition);

#endif /* BM_PM_H */
