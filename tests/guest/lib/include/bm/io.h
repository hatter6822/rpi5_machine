/*
 * MMIO and system register access for the bare-metal test library.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The library runs with the MMU off, so every data access is to Device
 * memory: accesses must be naturally aligned and exclusives are not
 * available. Cores share state through single-writer variables only.
 */

#ifndef BM_IO_H
#define BM_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BIT(n)                  (1u << (n))
#define BIT64(n)                (1ull << (n))
#define ARRAY_SIZE(a)           (sizeof(a) / sizeof((a)[0]))

static inline void mmio_write32(uintptr_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

static inline uint32_t mmio_read32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void mmio_write8(uintptr_t addr, uint8_t val)
{
    *(volatile uint8_t *)addr = val;
}

static inline uint8_t mmio_read8(uintptr_t addr)
{
    return *(volatile uint8_t *)addr;
}

#define read_sysreg(reg) ({                                 \
    uint64_t _val;                                          \
    __asm__ volatile("mrs %0, " #reg : "=r"(_val));         \
    _val;                                                   \
})

#define write_sysreg(reg, val) do {                         \
    uint64_t _val = (val);                                  \
    __asm__ volatile("msr " #reg ", %0" : : "r"(_val));     \
} while (0)

static inline void isb(void)
{
    __asm__ volatile("isb" ::: "memory");
}

static inline void dsb_sy(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void sev(void)
{
    __asm__ volatile("dsb sy; sev" ::: "memory");
}

static inline void wfe(void)
{
    __asm__ volatile("wfe" ::: "memory");
}

static inline void wfi(void)
{
    __asm__ volatile("wfi" ::: "memory");
}

static inline void cpu_relax(void)
{
    __asm__ volatile("yield" ::: "memory");
}

static inline unsigned current_el(void)
{
    return (read_sysreg(CurrentEL) >> 2) & 3;
}

/* Cortex-A76 reports its core number in MPIDR_EL1.Aff1 */
static inline unsigned this_core(void)
{
    return (read_sysreg(mpidr_el1) >> 8) & 0xff;
}

static inline void irq_unmask(void)
{
    __asm__ volatile("msr daifclr, #3" ::: "memory");   /* I and F */
}

static inline void irq_mask(void)
{
    __asm__ volatile("msr daifset, #3" ::: "memory");
}

#endif /* BM_IO_H */
