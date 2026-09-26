/*
 * Start-up, exit and platform discovery.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/console.h>
#include <bm/exception.h>
#include <bm/fdt.h>
#include <bm/gic.h>
#include <bm/io.h>
#include <bm/psci.h>
#include <bm/runtime.h>

/* raspi5b addresses, used when there is no device tree (bcm2712.dtsi) */
#define DEFAULT_UART10          0x107d001000ul
#define DEFAULT_GICD            0x107fff9000ul
#define DEFAULT_GICC            0x107fffa000ul
#define DEFAULT_SYSTIMER        0x107c003000ul
#define DEFAULT_SYSTIMER_SPI    64

/* Where QEMU places a -dtb blob for an ELF image, which gets no x0 */
#define RAM_BASE                0x0ul

#define SEMIHOST_SYS_EXIT       0x18u
#define ADP_STOPPED_APP_EXIT    0x20026u

struct bm_platform bm_plat;
void (*bm_panic_hook)(void);

extern char secondary_entry[];
static void (*volatile secondary_fn[BM_MAX_CPUS])(unsigned core);

static bool dt_pl011(int node, uintptr_t *base)
{
    uint64_t addr, size;

    if (!fdt_node_is_compatible(node, "arm,pl011") ||
        !fdt_reg(node, 0, &addr, &size)) {
        return false;
    }
    *base = addr;
    return true;
}

static void discover_uart(void)
{
    bm_plat.uart = DEFAULT_UART10;
    bm_plat.uart_from_dt = dt_pl011(fdt_stdout_offset(), &bm_plat.uart);
}

static void discover_gic(void)
{
    int node = fdt_find_compatible("arm,gic-400");
    uint64_t d, c, size;

    bm_plat.gicd = DEFAULT_GICD;
    bm_plat.gicc = DEFAULT_GICC;
    if (node >= 0 && fdt_reg(node, 0, &d, &size) &&
        fdt_reg(node, 1, &c, &size)) {
        bm_plat.gicd = d;
        bm_plat.gicc = c;
        bm_plat.gic_from_dt = true;
    }
}

static void discover_systimer(void)
{
    int node = fdt_find_compatible("brcm,bcm2835-system-timer");
    unsigned intid[BM_SYSTIMER_COMPARATORS];
    uint64_t addr, size;
    bool ok;

    bm_plat.systimer = DEFAULT_SYSTIMER;
    for (unsigned i = 0; i < BM_SYSTIMER_COMPARATORS; i++) {
        bm_plat.systimer_intid[i] = GIC_SPI(DEFAULT_SYSTIMER_SPI + i);
    }
    ok = node >= 0 && fdt_node_is_enabled(node) &&
         fdt_reg(node, 0, &addr, &size);
    for (unsigned i = 0; ok && i < BM_SYSTIMER_COMPARATORS; i++) {
        ok = fdt_gic_intid(node, i, &intid[i]);
    }
    if (ok) {
        bm_plat.systimer = addr;
        for (unsigned i = 0; i < BM_SYSTIMER_COMPARATORS; i++) {
            bm_plat.systimer_intid[i] = intid[i];
        }
        bm_plat.systimer_from_dt = true;
    }
}

/* Cores the tree describes as usable (QEMU marks absent ones "fail") */
static void discover_cpus(void)
{
    static const char *const names[BM_MAX_CPUS] = {
        "/cpus/cpu@0", "/cpus/cpu@1", "/cpus/cpu@2", "/cpus/cpu@3",
    };

    bm_plat.num_cpus = BM_MAX_CPUS;
    if (fdt_path_offset("/cpus/cpu@0") < 0) {
        return;
    }
    bm_plat.num_cpus = 0;
    for (unsigned i = 0; i < BM_MAX_CPUS; i++) {
        int node = fdt_path_offset(names[i]);

        if (node >= 0 && fdt_node_is_enabled(node)) {
            bm_plat.num_cpus++;
        }
    }
}

/* Called by start.S on core 0; @x0 is the boot register x0 */
void bm_start(uintptr_t x0);
void bm_start(uintptr_t x0)
{
    if ((x0 && fdt_init((const void *)x0)) ||
        fdt_init((const void *)RAM_BASE)) {
        bm_plat.has_dtb = true;
        bm_plat.dtb = fdt_address();
    }
    discover_uart();
    discover_gic();
    discover_systimer();
    discover_cpus();

    console_init(bm_plat.uart);
    gic_init_dist(bm_plat.gicd, bm_plat.gicc);
    gic_init_cpu();

    bm_exit(bm_main());
}

/* Called by start.S on a secondary core started by bm_start_core() */
void bm_secondary_start(unsigned core);
void bm_secondary_start(unsigned core)
{
    gic_init_cpu();
    secondary_fn[core](core);
    if (current_el() < 3) {
        psci_call(PSCI_CPU_OFF, 0, 0, 0);
    }
    for (;;) {
        wfe();
    }
}

int64_t bm_start_core(unsigned core, void (*fn)(unsigned core))
{
    if (core == 0 || core >= BM_MAX_CPUS) {
        return PSCI_INVALID_PARAMS;
    }
    secondary_fn[core] = fn;
    dsb_sy();
    return psci_cpu_on((uint64_t)core << 8, (uintptr_t)secondary_entry, core);
}

/* On hardware HLT is undefined unless halting debug is on: step over it */
static bool skip_undefined_hlt(struct exc_frame *frame, uint64_t esr,
                               uint64_t far)
{
    (void)far;
    if (ESR_EC(esr) != 0) {
        return false;
    }
    frame->elr += 4;
    return true;
}

static void semihost_exit(uint64_t code)
{
    static volatile uint64_t block[2];
    register uint64_t x0 __asm__("x0") = SEMIHOST_SYS_EXIT;
    register uint64_t x1 __asm__("x1") = (uintptr_t)block;

    block[0] = ADP_STOPPED_APP_EXIT;
    block[1] = code;
    exc_set_sync_hook(skip_undefined_hlt);
    __asm__ volatile("hlt #0xf000" : "+r"(x0) : "r"(x1) : "memory");
    exc_set_sync_hook(NULL);
}

void bm_exit(int code)
{
    irq_mask();
    if (current_el() == 3) {
        semihost_exit(code);
    } else {
        psci_call(PSCI_SYSTEM_OFF, 0, 0, 0);
    }
    for (;;) {
        wfe();
    }
}

void bm_panic(const char *fmt, ...)
{
    static volatile bool panicking;
    va_list ap;

    irq_mask();
    if (panicking) {
        bm_exit(3);
    }
    panicking = true;
    console_puts("PANIC: ");
    va_start(ap, fmt);
    bm_vprintf(fmt, ap);
    va_end(ap);
    console_puts("\n");
    if (bm_panic_hook) {
        bm_panic_hook();
    }
    bm_exit(2);
}
