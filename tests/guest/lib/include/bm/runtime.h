/*
 * Start-up, exit and platform discovery.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * start.S enters bm_start() on core 0 with a stack, the exception vectors
 * installed and interrupts routed to the current exception level. It
 * finds a device tree, sets up the console and the GIC from it (or from
 * built-in raspi5b addresses without one) and calls the program's
 * bm_main(). Returning from bm_main() ends the program.
 */

#ifndef BM_RUNTIME_H
#define BM_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#define BM_MAX_CPUS             4
#define BM_SYSTIMER_COMPARATORS 4

/* Where each address came from: the device tree, or the built-in default */
struct bm_platform {
    bool has_dtb;
    uintptr_t dtb;
    uintptr_t uart;
    bool uart_from_dt;
    uintptr_t gicd, gicc;
    bool gic_from_dt;
    uintptr_t systimer;
    unsigned systimer_intid[BM_SYSTIMER_COMPARATORS];
    bool systimer_from_dt;
    uintptr_t pm;               /* power management and watchdog */
    bool pm_from_dt;
    unsigned num_cpus;          /* from the DT; BM_MAX_CPUS without one */
};

extern struct bm_platform bm_plat;

/* Provided by the program */
int bm_main(void);

/*
 * Leave the program: PSCI SYSTEM_OFF below EL3, semihosting SYS_EXIT with
 * @code at EL3 (QEMU, -semihosting). Where neither ends the program, as on
 * hardware at EL3, the core parks.
 */
void __attribute__((noreturn)) bm_exit(int code);

/* Print "PANIC: <message>", call bm_panic_hook and exit with status 2 */
void __attribute__((noreturn, format(printf, 1, 2)))
bm_panic(const char *fmt, ...);

/* Called by bm_panic() after its message, so a test runner can report */
extern void (*bm_panic_hook)(void);

/*
 * This boot's number: 1 after power-on, one more after each system reset
 * (the watchdog, PSCI SYSTEM_RESET, QEMU's system_reset). RAM survives a
 * reset, and the program starts again from its entry point.
 */
unsigned bm_boot_count(void);

/*
 * BM_PERSIST_WORDS words the program keeps across system resets: zero at
 * power-on, left alone by the boots after it.
 */
#define BM_PERSIST_WORDS        64
uint64_t *bm_persistent(void);

/*
 * Start @fn(core) on secondary @core with PSCI CPU_ON: the core gets its
 * stack, vectors, interrupt routing and GIC CPU interface first. Returns
 * the PSCI status.
 */
int64_t bm_start_core(unsigned core, void (*fn)(unsigned core));

#endif /* BM_RUNTIME_H */
