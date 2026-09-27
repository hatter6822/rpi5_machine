/*
 * GICv2 (GIC-400) driver.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register layout from the Arm GIC Architecture Specification v2 (IHI
 * 0048B), chapter 4. All interrupts use Group 0, delivered as IRQ
 * (GICC_CTLR.FIQEn = 0), so the same code runs at EL2 without Security
 * Extensions and at EL3 with them. Below firmware that owns EL3, such as
 * TF-A, it sees the Non-secure copies of the registers: interrupts stay
 * in the Group 1 the firmware gave them, the group registers read as
 * zero and ignore writes, and GICC_PMR has one priority bit fewer.
 */

#include <bm/gic.h>
#include <bm/io.h>

#define GICD_CTLR               0x000
#define GICD_TYPER              0x004
#define GICD_IGROUPR(n)         (0x080 + 4 * (n))
#define GICD_ISENABLER(n)       (0x100 + 4 * (n))
#define GICD_ICENABLER(n)       (0x180 + 4 * (n))
#define GICD_ISPENDR(n)         (0x200 + 4 * (n))
#define GICD_ICPENDR(n)         (0x280 + 4 * (n))
#define GICD_ICACTIVER(n)       (0x380 + 4 * (n))
#define GICD_IPRIORITYR(n)      (0x400 + (n))
#define GICD_ITARGETSR(n)       (0x800 + (n))
#define GICD_ICFGR(n)           (0xc00 + 4 * (n))
#define GICD_SGIR               0xf00

#define GICC_CTLR               0x000
#define GICC_PMR                0x004
#define GICC_BPR                0x008
#define GICC_IAR                0x00c
#define GICC_EOIR               0x010

#define GICD_TYPER_SECURITYEXTN BIT(10)

#define GICD_CTLR_ENABLE        (BIT(0) | BIT(1))       /* Group 0 and 1 */
#define GICC_CTLR_ENABLE        (BIT(0) | BIT(1))       /* FIQEn = 0 */
#define GICC_CTLR_FIQEN         BIT(3)

static uintptr_t dist, cpuif;
static unsigned num_irqs;

void gic_init_dist(uintptr_t gicd, uintptr_t gicc)
{
    dist = gicd;
    cpuif = gicc;
    num_irqs = 32 * ((mmio_read32(dist + GICD_TYPER) & 0x1f) + 1);
    if (num_irqs > 1020) {
        num_irqs = 1020;
    }

    mmio_write32(dist + GICD_CTLR, 0);
    for (unsigned i = 32; i < num_irqs; i += 32) {
        mmio_write32(dist + GICD_ICENABLER(i / 32), ~0u);
        mmio_write32(dist + GICD_ICPENDR(i / 32), ~0u);
        mmio_write32(dist + GICD_ICACTIVER(i / 32), ~0u);
        mmio_write32(dist + GICD_IGROUPR(i / 32), 0);
    }
    for (unsigned i = 32; i < num_irqs; i += 4) {
        mmio_write32(dist + GICD_IPRIORITYR(i), GIC_PRIO_DEFAULT * 0x01010101u);
        mmio_write32(dist + GICD_ITARGETSR(i), 0x01010101u);
    }
    for (unsigned i = 32; i < num_irqs; i += 16) {
        mmio_write32(dist + GICD_ICFGR(i / 16), 0);
    }
    mmio_write32(dist + GICD_CTLR, GICD_CTLR_ENABLE);
}

void gic_init_cpu(void)
{
    /* SGIs and PPIs are banked per core */
    mmio_write32(dist + GICD_ICENABLER(0), ~0u);
    mmio_write32(dist + GICD_ICPENDR(0), ~0u);
    mmio_write32(dist + GICD_ICACTIVER(0), ~0u);
    mmio_write32(dist + GICD_IGROUPR(0), 0);
    for (unsigned i = 0; i < 32; i += 4) {
        mmio_write32(dist + GICD_IPRIORITYR(i), GIC_PRIO_DEFAULT * 0x01010101u);
    }

    mmio_write32(cpuif + GICC_PMR, 0xff);   /* unmask every priority */
    mmio_write32(cpuif + GICC_BPR, 0);
    mmio_write32(cpuif + GICC_CTLR, GICC_CTLR_ENABLE);
}

unsigned gic_num_irqs(void)
{
    return num_irqs;
}

bool gic_has_security_extensions(void)
{
    return mmio_read32(dist + GICD_TYPER) & GICD_TYPER_SECURITYEXTN;
}

/* Priority bits implemented: the ones that read back from GICC_PMR */
unsigned gic_priority_bits(void)
{
    uint32_t pmr = mmio_read32(cpuif + GICC_PMR);
    unsigned bits;

    mmio_write32(cpuif + GICC_PMR, 0xff);
    bits = __builtin_popcount(mmio_read32(cpuif + GICC_PMR));
    mmio_write32(cpuif + GICC_PMR, pmr);
    return bits;
}

void gic_enable(unsigned intid)
{
    mmio_write32(dist + GICD_ISENABLER(intid / 32), BIT(intid % 32));
}

void gic_disable(unsigned intid)
{
    mmio_write32(dist + GICD_ICENABLER(intid / 32), BIT(intid % 32));
}

bool gic_is_pending(unsigned intid)
{
    return mmio_read32(dist + GICD_ISPENDR(intid / 32)) & BIT(intid % 32);
}

void gic_clear_pending(unsigned intid)
{
    mmio_write32(dist + GICD_ICPENDR(intid / 32), BIT(intid % 32));
}

/* Byte-wide fields, updated with aligned word accesses */
static void write_byte_field(uintptr_t base, unsigned n, uint8_t val)
{
    uintptr_t word = base + (n & ~3u);
    unsigned shift = 8 * (n & 3);
    uint32_t v = mmio_read32(word);

    v = (v & ~(0xffu << shift)) | ((uint32_t)val << shift);
    mmio_write32(word, v);
}

void gic_set_priority(unsigned intid, uint8_t prio)
{
    write_byte_field(dist + GICD_IPRIORITYR(0), intid, prio);
}

void gic_set_target(unsigned spi_intid, uint8_t cpu_mask)
{
    write_byte_field(dist + GICD_ITARGETSR(0), spi_intid, cpu_mask);
}

void gic_set_group(unsigned intid, unsigned group)
{
    uintptr_t reg = dist + GICD_IGROUPR(intid / 32);
    uint32_t v = mmio_read32(reg);

    mmio_write32(reg, group ? v | BIT(intid % 32) : v & ~BIT(intid % 32));
}

uint32_t gic_read_group_reg(unsigned intid)
{
    return mmio_read32(dist + GICD_IGROUPR(intid / 32));
}

void gic_group0_fiq(bool fiq)
{
    uint32_t v = mmio_read32(cpuif + GICC_CTLR);

    mmio_write32(cpuif + GICC_CTLR, fiq ? v | GICC_CTLR_FIQEN
                                        : v & ~GICC_CTLR_FIQEN);
}

void gic_set_edge(unsigned intid, bool edge)
{
    uintptr_t reg = dist + GICD_ICFGR(intid / 16);
    unsigned bit = 2 * (intid % 16) + 1;
    uint32_t v = mmio_read32(reg);

    mmio_write32(reg, edge ? v | BIT(bit) : v & ~BIT(bit));
}

void gic_send_sgi(unsigned intid, enum gic_sgi_filter filter,
                  uint8_t cpu_mask)
{
    dsb_sy();   /* make prior writes visible to the target first */
    mmio_write32(dist + GICD_SGIR, ((uint32_t)filter << 24) |
                 ((uint32_t)cpu_mask << 16) | (intid & 0xf));
}

uint32_t gic_ack(void)
{
    return mmio_read32(cpuif + GICC_IAR);
}

void gic_eoi(uint32_t iar)
{
    mmio_write32(cpuif + GICC_EOIR, iar);
}
