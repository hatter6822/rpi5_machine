/*
 * Running code in the Non-secure world, for a guest that owns EL3.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/io.h>
#include <bm/runtime.h>
#include <bm/world.h>

#define SCR_NS                  (1u << 0)
#define SCR_HCE                 (1u << 8)
#define SCR_RW                  (1u << 10)
#define HCR_FMO                 (1ull << 3)
#define HCR_IMO                 (1ull << 4)
#define HCR_AMO                 (1ull << 5)
#define HCR_RW                  (1ull << 31)
#define SCTLR_EL2_RES1          0x30c50830ull   /* MMU and caches off */
#define SPSR_EL2H               0x9
#define SPSR_DAIF               (0xfu << 6)

void world_enter(void (*fn)(void), uintptr_t sp_el2, uint64_t spsr);
extern char bm_vectors[];

void bm_run_nonsecure_el2(void (*fn)(void), void *stack_top, uint32_t route)
{
    uint64_t scr = read_sysreg(scr_el3);

    if (current_el() != 3 || this_core() != 0) {
        bm_panic("bm_run_nonsecure_el2: EL3 on core 0 only");
    }
    irq_mask();
    write_sysreg(vbar_el2, (uintptr_t)bm_vectors);
    write_sysreg(sctlr_el2, SCTLR_EL2_RES1);
    /* Without {I,F,A}MO they would target EL1, and be masked at EL2 */
    write_sysreg(hcr_el2, HCR_RW | HCR_AMO | HCR_IMO | HCR_FMO);
    write_sysreg(scr_el3, SCR_NS | SCR_HCE | SCR_RW |
                          (route & (SCR_IRQ | SCR_FIQ | SCR_EA)));
    isb();
    world_enter(fn, (uintptr_t)stack_top, SPSR_EL2H | SPSR_DAIF);
    write_sysreg(scr_el3, scr);
    isb();
}
