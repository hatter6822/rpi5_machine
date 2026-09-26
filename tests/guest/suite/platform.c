/*
 * Platform discovery and PSCI tests.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/fdt.h>
#include <bm/psci.h>
#include <bm/runtime.h>
#include <bm/test.h>
#include <bm/timer.h>

/* With a device tree, every device the suite uses is found in it */
TEST(platform_dt, "platform/device-tree")
{
    if (!bm_plat.has_dtb) {
        SKIP("no device tree");
    }
    ASSERT(bm_plat.uart_from_dt);
    ASSERT(bm_plat.gic_from_dt);
    ASSERT(bm_plat.systimer_from_dt);
    ASSERT_EQ(bm_plat.uart, 0x107d001000);
    ASSERT_EQ(bm_plat.gicd, 0x107fff9000);
    ASSERT_EQ(bm_plat.gicc, 0x107fffa000);
    ASSERT_EQ(bm_plat.systimer, 0x107c003000);
    ASSERT_EQ(bm_plat.systimer_intid[0], 96);
}

TEST(psci_version, "psci/version")
{
    if (current_el() == 3) {
        SKIP("no PSCI below a guest that owns EL3");
    }
    int64_t v = psci_call(PSCI_VERSION, 0, 0, 0);

    ASSERT_GE(v, 0x10000);      /* 1.0 or later */
}

static volatile uint64_t checked_in[BM_MAX_CPUS];

static void report(unsigned core)
{
    checked_in[core] = read_sysreg(mpidr_el1) | BIT64(63);
    sev();
}

/*
 * Every core starts with PSCI CPU_ON and turns itself off again. Cores
 * the device tree marks absent (-smp) are refused; without a tree, the
 * first refusal marks the end of the cores that exist.
 */
TEST(smp_cpu_on, "smp/cpu-on")
{
    if (current_el() == 3) {
        SKIP("no PSCI below a guest that owns EL3");
    }
    for (unsigned core = 1; core < BM_MAX_CPUS; core++) {
        uint64_t mpidr = (uint64_t)core << 8;
        int64_t ret;

        checked_in[core] = 0;
        ret = bm_start_core(core, report);
        if (bm_plat.has_dtb && core >= bm_plat.num_cpus) {
            ASSERT_MSG(ret == PSCI_INVALID_PARAMS,
                       "absent core %u: CPU_ON returned %ld", core, ret);
            continue;
        }
        if (!bm_plat.has_dtb && ret == PSCI_INVALID_PARAMS) {
            bm_test_note("smp/cpu-on: %u cores", core);
            break;
        }
        ASSERT_MSG(ret == PSCI_SUCCESS, "core %u: CPU_ON returned %ld", core,
                   ret);
        ASSERT_MSG(wait_until(checked_in[core], 1000000),
                   "core %u did not check in", core);
        ASSERT_EQ(checked_in[core] & 0xffffff, mpidr);
        ASSERT_MSG(wait_until(psci_call(PSCI_AFFINITY_INFO_64, mpidr, 0,
                                        0) == 1, 1000000),
                   "core %u did not turn off", core);
    }
}
