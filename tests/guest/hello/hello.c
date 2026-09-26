/*
 * raspi5b bare-metal smoke test.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exercises the minimum a microkernel needs from the machine: the UART10
 * PL011, the generic timer frequency, the boot exception level and, when
 * the machine provides it, PSCI CPU_ON/SYSTEM_OFF. With the MMU off all
 * data accesses are Device memory, so cores communicate through plain
 * aligned stores rather than exclusives.
 */

#include <stdbool.h>
#include <stdint.h>

#define NUM_CPUS                4

#define UART10_BASE             0x107d001000UL
#define UART_DR                 0x00
#define UART_FR                 0x18
#define UART_IBRD               0x24
#define UART_FBRD               0x28
#define UART_LCRH               0x2c
#define UART_CR                 0x30
#define UART_FR_TXFF            (1u << 5)
#define UART_FR_BUSY            (1u << 3)
#define UART_LCRH_WLEN_8        (3u << 5)
#define UART_LCRH_FEN           (1u << 4)
#define UART_CR_UARTEN          (1u << 0)
#define UART_CR_TXE             (1u << 8)
#define UART_CR_RXE             (1u << 9)
#define UART_CLOCK_HZ           9216000u
#define UART_BAUD               115200u

#define PSCI_VERSION            0x84000000u
#define PSCI_CPU_ON_64          0xc4000003u
#define PSCI_SYSTEM_OFF         0x84000008u

#define SEMIHOST_SYS_EXIT       0x18u
#define ADP_STOPPED_APP_EXIT    0x20026u

extern char secondary_entry[];

/* Written by each secondary core once it runs, read by core 0 */
static volatile uint64_t core_report[NUM_CPUS];

static inline void mmio_write32(uintptr_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

static inline uint32_t mmio_read32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

#define read_sysreg(reg) ({                                 \
    uint64_t _val;                                          \
    __asm__ volatile("mrs %0, " #reg : "=r"(_val));         \
    _val;                                                   \
})

static unsigned current_el(void)
{
    return (read_sysreg(CurrentEL) >> 2) & 3;
}

static void uart_init(void)
{
    uint32_t div64 = (4 * UART_CLOCK_HZ + UART_BAUD / 2) / UART_BAUD;

    mmio_write32(UART10_BASE + UART_CR, 0);
    while (mmio_read32(UART10_BASE + UART_FR) & UART_FR_BUSY) {
    }
    mmio_write32(UART10_BASE + UART_IBRD, div64 >> 6);
    mmio_write32(UART10_BASE + UART_FBRD, div64 & 0x3f);
    mmio_write32(UART10_BASE + UART_LCRH, UART_LCRH_WLEN_8 | UART_LCRH_FEN);
    mmio_write32(UART10_BASE + UART_CR,
                 UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);
}

static void uart_putc(char c)
{
    while (mmio_read32(UART10_BASE + UART_FR) & UART_FR_TXFF) {
    }
    mmio_write32(UART10_BASE + UART_DR, (uint8_t)c);
}

static void uart_puts(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s);
    }
}

static void put_dec(uint64_t v)
{
    char buf[21];
    int i = sizeof(buf);

    buf[--i] = '\0';
    do {
        buf[--i] = '0' + v % 10;
        v /= 10;
    } while (v);
    uart_puts(&buf[i]);
}

static void put_hex(uint64_t v)
{
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        uart_putc("0123456789abcdef"[(v >> shift) & 0xf]);
    }
}

/* SMC Calling Convention: x4-x17 may be clobbered by the callee */
static int64_t psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;

    __asm__ volatile("smc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     :
                     : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                       "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    return (int64_t)x0;
}

static __attribute__((noreturn)) void semihost_exit(uint64_t code)
{
    static volatile uint64_t block[2];
    register uint64_t x0 __asm__("x0") = SEMIHOST_SYS_EXIT;
    register uint64_t x1 __asm__("x1") = (uintptr_t)block;

    block[0] = ADP_STOPPED_APP_EXIT;
    block[1] = code;
    __asm__ volatile("hlt #0xf000" : : "r"(x0), "r"(x1) : "memory");
    for (;;) {
    }
}

static bool wait_for_core(unsigned core)
{
    for (uint64_t spins = 0; spins < 100000000; spins++) {
        if (core_report[core]) {
            return true;
        }
    }
    return false;
}

void secondary_main(uint64_t core);
void secondary_main(uint64_t core)
{
    core_report[core] = read_sysreg(mpidr_el1) | (1ull << 32);
    __asm__ volatile("dsb sy; sev" ::: "memory");
}

void primary_main(void);
void primary_main(void)
{
    unsigned el = current_el();

    uart_init();
    uart_puts("raspi5b: core 0 up at EL");
    put_dec(el);
    uart_puts(", MPIDR ");
    put_hex(read_sysreg(mpidr_el1));
    uart_puts(", CNTFRQ ");
    put_dec(read_sysreg(cntfrq_el0));
    uart_puts(" Hz\n");

    if (el == 3) {
        /* secure=on: no firmware below us, so no PSCI to call */
        uart_puts("raspi5b: EL3 owned by guest, exiting via semihosting\n");
        semihost_exit(0);
    }

    int64_t version = psci_call(PSCI_VERSION, 0, 0, 0);
    uart_puts("raspi5b: PSCI ");
    put_dec((uint64_t)version >> 16);
    uart_puts(".");
    put_dec(version & 0xffff);
    uart_puts("\n");

    for (unsigned core = 1; core < NUM_CPUS; core++) {
        int64_t ret = psci_call(PSCI_CPU_ON_64, core << 8,
                                (uintptr_t)secondary_entry, core);

        uart_puts("raspi5b: core ");
        put_dec(core);
        if (ret != 0) {
            uart_puts(" CPU_ON failed: ");
            put_dec((uint64_t)-ret);
            uart_puts("\n");
        } else if (!wait_for_core(core)) {
            uart_puts(" did not check in\n");
        } else {
            uart_puts(" online, MPIDR ");
            put_hex(core_report[core] & 0xffffffffu);
            uart_puts("\n");
        }
    }

    uart_puts("raspi5b: PSCI SYSTEM_OFF\n");
    psci_call(PSCI_SYSTEM_OFF, 0, 0, 0);
    uart_puts("raspi5b: SYSTEM_OFF returned!\n");
}
