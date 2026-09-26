/*
 * Exception and interrupt dispatch, called from the vectors in start.S.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/console.h>
#include <bm/exception.h>
#include <bm/gic.h>
#include <bm/io.h>
#include <bm/runtime.h>

#define MAX_INTID               1020

static struct {
    irq_handler_t fn;
    void *arg;
} irq_table[MAX_INTID];

static volatile sync_hook_t sync_hook;

void irq_register(unsigned intid, irq_handler_t fn, void *arg)
{
    if (intid < MAX_INTID) {
        irq_table[intid].arg = arg;
        irq_table[intid].fn = fn;
    }
}

void irq_unregister(unsigned intid)
{
    irq_register(intid, NULL, NULL);
}

void exc_set_sync_hook(sync_hook_t hook)
{
    sync_hook = hook;
}

uint64_t exc_esr(void)
{
    switch (current_el()) {
    case 3:
        return read_sysreg(esr_el3);
    case 2:
        return read_sysreg(esr_el2);
    default:
        return read_sysreg(esr_el1);
    }
}

uint64_t exc_far(void)
{
    switch (current_el()) {
    case 3:
        return read_sysreg(far_el3);
    case 2:
        return read_sysreg(far_el2);
    default:
        return read_sysreg(far_el1);
    }
}

static void handle_irq(void)
{
    for (;;) {
        uint32_t iar = gic_ack();
        unsigned intid = iar & 0x3ff;

        if (intid >= MAX_INTID) {
            return;             /* spurious: nothing (more) pending */
        }
        if (irq_table[intid].fn) {
            irq_table[intid].fn(intid, irq_table[intid].arg);
        } else {
            bm_panic("unexpected interrupt %u on core %u", intid,
                     this_core());
        }
        gic_eoi(iar);
    }
}

void exception_dispatch(struct exc_frame *frame, uint64_t vector);
void exception_dispatch(struct exc_frame *frame, uint64_t vector)
{
    static const char *const kinds[] = { "sync", "IRQ", "FIQ", "SError" };
    enum exc_kind kind = vector & 3;
    enum exc_source source = vector >> 2;
    uint64_t esr = exc_esr(), far = exc_far();

    if (source == EXC_FROM_CURRENT_SPX &&
        (kind == EXC_IRQ || kind == EXC_FIQ)) {
        handle_irq();
        return;
    }
    if (source == EXC_FROM_CURRENT_SPX && kind == EXC_SYNC && sync_hook &&
        sync_hook(frame, esr, far)) {
        return;
    }
    bm_panic("unexpected %s exception at EL%u on core %u: ESR 0x%lx "
             "ELR 0x%lx FAR 0x%lx SPSR 0x%lx (vector group %u)",
             kinds[kind], current_el(), this_core(), esr, frame->elr, far,
             frame->spsr, (unsigned)source);
}
