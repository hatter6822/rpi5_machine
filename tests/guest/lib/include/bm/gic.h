/*
 * GICv2 (GIC-400) driver.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef BM_GIC_H
#define BM_GIC_H

#include <stdbool.h>
#include <stdint.h>

#define GIC_SGI(n)              (n)
#define GIC_PPI(n)              (16 + (n))
#define GIC_SPI(n)              (32 + (n))
#define GIC_SPURIOUS            1023

/* Priority given to every interrupt by gic_init_*() */
#define GIC_PRIO_DEFAULT        0xa0

/* Target list filters of GICD_SGIR */
enum gic_sgi_filter {
    GIC_SGI_TO_LIST   = 0,
    GIC_SGI_TO_OTHERS = 1,
    GIC_SGI_TO_SELF   = 2,
};

/*
 * Distributor setup, once, on the boot core: every SPI disabled, not
 * pending, level-sensitive, Group 0, default priority, targeted at core 0.
 * With Security Extensions (EL3) Group 0 is Secure; without them all
 * interrupts are Non-secure.
 */
void gic_init_dist(uintptr_t gicd, uintptr_t gicc);

/* Banked per-core setup: SGIs/PPIs and the CPU interface of this core */
void gic_init_cpu(void);

unsigned gic_num_irqs(void);
unsigned gic_priority_bits(void);

void gic_enable(unsigned intid);
void gic_disable(unsigned intid);
bool gic_is_pending(unsigned intid);
void gic_clear_pending(unsigned intid);
void gic_set_priority(unsigned intid, uint8_t prio);
void gic_set_target(unsigned spi_intid, uint8_t cpu_mask);
void gic_set_edge(unsigned intid, bool edge);

/*
 * With Security Extensions, from the Secure side: put @intid in Group 0
 * (Secure) or Group 1 (Non-secure), and signal Group 0 as FIQ instead of
 * IRQ on this core (GICC_CTLR.FIQEn). gic_read_group_reg() returns the
 * GICD_IGROUPR word holding @intid as the caller's world sees it.
 */
void gic_set_group(unsigned intid, unsigned group);
uint32_t gic_read_group_reg(unsigned intid);
void gic_group0_fiq(bool fiq);
void gic_send_sgi(unsigned intid, enum gic_sgi_filter filter,
                  uint8_t cpu_mask);

/* Acknowledge the highest-priority pending interrupt; GIC_SPURIOUS if none */
uint32_t gic_ack(void);
void gic_eoi(uint32_t iar);

#endif /* BM_GIC_H */
