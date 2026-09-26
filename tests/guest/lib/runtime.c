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
#include <bm/string.h>

/* raspi5b addresses, used when there is no device tree (bcm2712.dtsi) */
#define DEFAULT_UART10          0x107d001000ul
#define DEFAULT_UART10_SPI      121
#define DEFAULT_GICD            0x107fff9000ul
#define DEFAULT_GICC            0x107fffa000ul
#define DEFAULT_SYSTIMER        0x107c003000ul
#define DEFAULT_SYSTIMER_SPI    64
#define DEFAULT_PM              0x107d200000ul
#define DEFAULT_MBOX            0x107c013880ul
#define DEFAULT_RNG             0x107d208000ul

/* Where QEMU places a -dtb blob for an ELF image, which gets no x0 */
#define RAM_BASE                0x0ul

#define SEMIHOST_SYS_EXIT       0x18u
#define ADP_STOPPED_APP_EXIT    0x20026u

struct bm_platform bm_plat;
void (*bm_panic_hook)(void);

/* What survives a reset, at __persist_start (link.ld) */
#define PERSIST_MAGIC           0x74736973726570ull     /* "persist" */
#define PERSIST_SIZE            4096

struct persist {
    uint64_t magic;
    uint64_t boots;
    uint64_t words[BM_PERSIST_WORDS];
};

_Static_assert(sizeof(struct persist) <= PERSIST_SIZE, "see link.ld");

extern char __persist_start[];

extern char secondary_entry[];
static void (*volatile secondary_fn[BM_MAX_CPUS])(unsigned core);

/* At EL3: set to release a core from start.S's spin table */
extern volatile uint64_t secondary_release[BM_MAX_CPUS];

/* At EL3: set by bm_start_core(), cleared by the core when it is done */
static volatile bool secondary_busy[BM_MAX_CPUS];

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
    int node = fdt_stdout_offset();
    unsigned intid;

    bm_plat.uart = DEFAULT_UART10;
    bm_plat.uart_intid = GIC_SPI(DEFAULT_UART10_SPI);
    bm_plat.uart_from_dt = dt_pl011(node, &bm_plat.uart);
    if (bm_plat.uart_from_dt && fdt_gic_intid(node, 0, &intid)) {
        bm_plat.uart_intid = intid;
    }
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

static void discover_pm(void)
{
    /* Linux drivers/mfd/bcm2835-pm.c */
    static const char *const compatibles[] = {
        "brcm,bcm2712-pm", "brcm,bcm2711-pm", "brcm,bcm2835-pm",
        "brcm,bcm2835-pm-wdt",
    };
    uint64_t addr, size;

    bm_plat.pm = DEFAULT_PM;
    for (unsigned i = 0; i < sizeof(compatibles) / sizeof(*compatibles);
         i++) {
        int node = fdt_find_compatible(compatibles[i]);

        if (node >= 0 && fdt_node_is_enabled(node) &&
            fdt_reg(node, 0, &addr, &size)) {
            bm_plat.pm = addr;
            bm_plat.pm_from_dt = true;
            return;
        }
    }
}

static void discover_mbox(void)
{
    int node = fdt_find_compatible("brcm,bcm2835-mbox");
    uint64_t addr, size;

    bm_plat.mbox = DEFAULT_MBOX;
    if (node >= 0 && fdt_node_is_enabled(node) &&
        fdt_reg(node, 0, &addr, &size)) {
        bm_plat.mbox = addr;
        bm_plat.mbox_from_dt = true;
    }
}

static void discover_rng(void)
{
    int node = fdt_find_compatible("brcm,bcm2711-rng200");
    uint64_t addr, size;

    bm_plat.rng = DEFAULT_RNG;
    if (node >= 0 && fdt_node_is_enabled(node) &&
        fdt_reg(node, 0, &addr, &size)) {
        bm_plat.rng = addr;
        bm_plat.rng_from_dt = true;
    }
}

/*
 * Cores the tree describes as usable (QEMU marks absent ones "fail").
 * Without a tree, PSCI AFFINITY_INFO tells which exist; a guest that owns
 * EL3 has no PSCI, and assumes BM_MAX_CPUS.
 */
static void discover_cpus(void)
{
    int cpus = fdt_path_offset("/cpus");
    unsigned n = 0;

    if (!bm_plat.has_dtb) {
        n = BM_MAX_CPUS;
        for (unsigned core = 1; current_el() < 3 && core < n; core++) {
            if (psci_call(PSCI_AFFINITY_INFO_64, (uint64_t)core << 8, 0,
                          0) == PSCI_INVALID_PARAMS) {
                n = core;
            }
        }
        bm_plat.num_cpus = n;
        return;
    }

    /*
     * Node names vary (the firmware's tree has cpu@1 at reg 0x100, the
     * built-in one cpu@100), so go by device_type
     */
    for (int node = fdt_first_subnode(cpus); node >= 0;
         node = fdt_next_subnode(node)) {
        const char *type = fdt_getprop(node, "device_type", NULL);

        if (type && !strcmp(type, "cpu") && fdt_node_is_enabled(node)) {
            n++;
        }
    }
    bm_plat.num_cpus = n && n < BM_MAX_CPUS ? n : BM_MAX_CPUS;
}

static struct persist *persist(void)
{
    return (struct persist *)__persist_start;
}

/* Power-on leaves no magic (QEMU zeroes RAM; hardware leaves noise) */
static void count_boot(void)
{
    struct persist *p = persist();

    if (p->magic != PERSIST_MAGIC) {
        memset(p, 0, sizeof(*p));
        p->magic = PERSIST_MAGIC;
    }
    p->boots++;
}

unsigned bm_boot_count(void)
{
    return persist()->boots;
}

uint64_t *bm_persistent(void)
{
    return persist()->words;
}

/* Called by start.S on core 0; @x0 is the boot register x0 */
void bm_start(uintptr_t x0);
void bm_start(uintptr_t x0)
{
    count_boot();
    if ((x0 && fdt_init((const void *)x0)) ||
        fdt_init((const void *)RAM_BASE)) {
        bm_plat.has_dtb = true;
        bm_plat.dtb = fdt_address();
    }
    discover_uart();
    discover_gic();
    discover_systimer();
    discover_pm();
    discover_mbox();
    discover_rng();
    discover_cpus();

    if (bm_early) {
        bm_early();
    }
    console_init(bm_plat.uart);
    gic_init_dist(bm_plat.gicd, bm_plat.gicc);
    gic_init_cpu();

    bm_exit(bm_main());
}

/*
 * Called by start.S on a secondary core started by bm_start_core(). Below
 * EL3 the core turns itself off; at EL3 it returns to the spin table.
 */
void bm_secondary_start(unsigned core);
void bm_secondary_start(unsigned core)
{
    gic_init_cpu();
    secondary_fn[core](core);
    irq_mask();
    if (current_el() < 3) {
        psci_call(PSCI_CPU_OFF, 0, 0, 0);
        for (;;) {
            wfe();
        }
    }
    dsb_sy();
    secondary_busy[core] = false;
    sev();
}

int64_t bm_start_core(unsigned core, void (*fn)(unsigned core))
{
    if (core == 0 || core >= BM_MAX_CPUS) {
        return PSCI_INVALID_PARAMS;
    }
    if (current_el() < 3) {
        secondary_fn[core] = fn;
        dsb_sy();
        return psci_cpu_on((uint64_t)core << 8, (uintptr_t)secondary_entry,
                           core);
    }

    /* No PSCI below a guest that owns EL3: release it from the spin table */
    if (core >= bm_plat.num_cpus) {
        return PSCI_INVALID_PARAMS;
    }
    if (secondary_busy[core]) {
        return PSCI_ALREADY_ON;
    }
    secondary_busy[core] = true;
    secondary_fn[core] = fn;
    dsb_sy();
    secondary_release[core] = 1;
    sev();
    return PSCI_SUCCESS;
}

bool bm_core_is_off(unsigned core)
{
    if (current_el() < 3) {
        return psci_call(PSCI_AFFINITY_INFO_64, (uint64_t)core << 8, 0, 0) ==
               1;
    }
    return !secondary_busy[core];
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
