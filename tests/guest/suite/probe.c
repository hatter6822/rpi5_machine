/*
 * The identification registers of core 0, the GIC and the UART, as one
 * sorted "name=value" line each, so transcripts from QEMU and hardware
 * can be diffed. A first slice of the hardware probe (WS0.4), which adds
 * the other cores, the caches and every peripheral register.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/io.h>
#include <bm/runtime.h>
#include <bm/test.h>

/* Registers the assembler may not know by name, by encoding */
#define ID_AA64ISAR2_EL1        s3_0_c0_c6_2
#define ID_AA64MMFR2_EL1        s3_0_c0_c7_2

#define GICD_TYPER              0x004
#define GICD_IIDR               0x008
#define GICC_IIDR               0x0fc
#define PL011_PERIPHID(n)       (0xfe0 + 4 * (n))

#define MIDR_ARM_CORTEX_A76     0x410fd0b0u     /* without the revision */
#define MIDR_VARIANT_REVISION   0x00f0000fu

static uint32_t periph_id(uintptr_t base)
{
    uint32_t id = 0;

    for (unsigned n = 0; n < 4; n++) {
        id |= (mmio_read32(base + PL011_PERIPHID(n)) & 0xff) << (8 * n);
    }
    return id;
}

TEST(probe_dump, "probe/dump")
{
    const struct {
        const char *name;
        uint64_t value;
    } regs[] = {                        /* sorted by name */
        { "clidr_el1", read_sysreg(clidr_el1) },
        { "cntfrq_el0", read_sysreg(cntfrq_el0) },
        { "ctr_el0", read_sysreg(ctr_el0) },
        { "dczid_el0", read_sysreg(dczid_el0) },
        { "gicc_iidr", mmio_read32(bm_plat.gicc + GICC_IIDR) },
        { "gicd_iidr", mmio_read32(bm_plat.gicd + GICD_IIDR) },
        { "gicd_typer", mmio_read32(bm_plat.gicd + GICD_TYPER) },
        { "id_aa64dfr0_el1", read_sysreg(id_aa64dfr0_el1) },
        { "id_aa64isar0_el1", read_sysreg(id_aa64isar0_el1) },
        { "id_aa64isar1_el1", read_sysreg(id_aa64isar1_el1) },
        { "id_aa64isar2_el1", read_sysreg(ID_AA64ISAR2_EL1) },
        { "id_aa64mmfr0_el1", read_sysreg(id_aa64mmfr0_el1) },
        { "id_aa64mmfr1_el1", read_sysreg(id_aa64mmfr1_el1) },
        { "id_aa64mmfr2_el1", read_sysreg(ID_AA64MMFR2_EL1) },
        { "id_aa64pfr0_el1", read_sysreg(id_aa64pfr0_el1) },
        { "id_aa64pfr1_el1", read_sysreg(id_aa64pfr1_el1) },
        { "midr_el1", read_sysreg(midr_el1) },
        { "mpidr_el1", read_sysreg(mpidr_el1) },
        { "revidr_el1", read_sysreg(revidr_el1) },
        { "uart_periphid", periph_id(bm_plat.uart) },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(regs); i++) {
        bm_test_note("probe: %s=0x%lx", regs[i].name, regs[i].value);
    }
    ASSERT_EQ(read_sysreg(midr_el1) & ~MIDR_VARIANT_REVISION,
              MIDR_ARM_CORTEX_A76);
}
