/*
 * Cortex-A76 implementation tests: the IMPLEMENTATION DEFINED registers
 * firmware writes.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/exception.h>
#include <bm/io.h>
#include <bm/test.h>

/* The registers by encoding (Cortex-A76 TRM, AArch64 IMPDEF registers) */
#define CPUCFR_EL1              s3_0_c15_c0_0
#define CPUACTLR_EL1            s3_0_c15_c1_0
#define CPUACTLR2_EL1           s3_0_c15_c1_1
#define CPUACTLR3_EL1           s3_0_c15_c1_2
#define CPUECTLR_EL1            s3_0_c15_c1_4
#define CPUPWRCTLR_EL1          s3_0_c15_c2_7
#define CPUPSELR_EL3            s3_6_c15_c8_0
#define CPUPCR_EL3              s3_6_c15_c8_1
#define CPUPOR_EL3              s3_6_c15_c8_2
#define CPUPMR_EL3              s3_6_c15_c8_3

#define ESR_EC_UNKNOWN          0x00

static volatile unsigned undefs;

static bool count_undef(struct exc_frame *frame, uint64_t esr, uint64_t far)
{
    (void)far;
    if (ESR_EC(esr) != ESR_EC_UNKNOWN) {
        return false;
    }
    undefs++;
    frame->elr += 4;
    return true;
}

/*
 * read_sysreg() and write_sysreg() take a register's name as written, so
 * these add the expansion that turns the names above into encodings.
 * TOUCH() reads a register and stores the value back, which changes
 * nothing.
 */
#define READ(reg)               read_sysreg(reg)
#define WRITE(reg, val)         write_sysreg(reg, val)
#define TOUCH(reg)              write_sysreg(reg, read_sysreg(reg))

/*
 * None of the registers TF-A's cortex_a76 support writes (errata
 * workarounds, the core power-down sequence, the errata 1946160 patch
 * slots at EL3) is undefined at the exception level we run at.
 */
TEST(cpu_impdef_registers, "cpu/impdef-registers")
{
    undefs = 0;
    exc_set_sync_hook(count_undef);

    (void)READ(CPUCFR_EL1);
    TOUCH(CPUACTLR_EL1);
    TOUCH(CPUACTLR2_EL1);
    TOUCH(CPUACTLR3_EL1);
    TOUCH(CPUECTLR_EL1);
    TOUCH(CPUPWRCTLR_EL1);
    ASSERT_EQ(undefs, 0);

    if (current_el() == 3) {
        uint64_t slot = READ(CPUPSELR_EL3);

        TOUCH(CPUPCR_EL3);
        TOUCH(CPUPOR_EL3);
        TOUCH(CPUPMR_EL3);
        WRITE(CPUPSELR_EL3, slot);
        ASSERT_EQ(undefs, 0);
    }
}
