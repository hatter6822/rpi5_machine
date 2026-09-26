/*
 * raspi5b bare-metal smoke test.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exercises the minimum a microkernel needs from the machine: the UART10
 * PL011, the generic timer frequency, the boot exception level and, when
 * the machine provides it, PSCI CPU_ON/SYSTEM_OFF. Its transcript is
 * checked line by line by tests/smoke/test_hello.py.
 */

#include <bm/console.h>
#include <bm/io.h>
#include <bm/psci.h>
#include <bm/runtime.h>
#include <bm/timer.h>

/* Written by each secondary core once it runs, read by core 0 */
static volatile uint64_t core_report[BM_MAX_CPUS];

static void secondary_main(unsigned core)
{
    core_report[core] = read_sysreg(mpidr_el1) | BIT64(32);
    sev();
}

int bm_main(void)
{
    unsigned el = current_el();

    bm_printf("raspi5b: core 0 up at EL%u, MPIDR 0x%016lx, CNTFRQ %lu Hz\n",
              el, read_sysreg(mpidr_el1), counter_freq());

    if (el == 3) {
        /* secure=on: no firmware below us, so no PSCI to call */
        bm_printf("raspi5b: EL3 owned by guest, exiting via semihosting\n");
        return 0;
    }

    int64_t version = psci_call(PSCI_VERSION, 0, 0, 0);
    bm_printf("raspi5b: PSCI %lu.%lu\n", (uint64_t)version >> 16,
              (uint64_t)version & 0xffff);

    for (unsigned core = 1; core < BM_MAX_CPUS; core++) {
        int64_t ret = bm_start_core(core, secondary_main);

        if (ret != PSCI_SUCCESS) {
            bm_printf("raspi5b: core %u CPU_ON failed: %ld\n", core, -ret);
        } else if (!wait_until(core_report[core], 1000000)) {
            bm_printf("raspi5b: core %u did not check in\n", core);
        } else {
            bm_printf("raspi5b: core %u online, MPIDR 0x%016lx\n", core,
                      core_report[core] & 0xffffffffu);
        }
    }

    bm_printf("raspi5b: PSCI SYSTEM_OFF\n");
    psci_call(PSCI_SYSTEM_OFF, 0, 0, 0);
    bm_printf("raspi5b: SYSTEM_OFF returned!\n");
    return 1;
}
