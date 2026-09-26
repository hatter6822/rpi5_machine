/*
 * Exception vectors and interrupt dispatch.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef BM_EXCEPTION_H
#define BM_EXCEPTION_H

#include <stdbool.h>
#include <stdint.h>

/* Saved by the vectors in start.S; the layout is shared with them */
struct exc_frame {
    uint64_t x[31];
    uint64_t elr;
    uint64_t spsr;
    uint64_t pad;
};

enum exc_kind {
    EXC_SYNC,
    EXC_IRQ,
    EXC_FIQ,
    EXC_SERROR,
};

/* The vector table offset group the exception came through */
enum exc_source {
    EXC_FROM_CURRENT_SP0,
    EXC_FROM_CURRENT_SPX,
    EXC_FROM_LOWER_A64,
    EXC_FROM_LOWER_A32,
};

typedef void (*irq_handler_t)(unsigned intid, void *arg);

/*
 * Handlers run with interrupts masked, before the interrupt is ended with
 * GICC_EOIR. One table serves every core (SGIs and PPIs are banked in the
 * GIC, not here).
 */
void irq_register(unsigned intid, irq_handler_t fn, void *arg);
void irq_unregister(unsigned intid);

/*
 * An expected synchronous exception: the hook returns true when it has
 * handled the exception (adjusting frame->elr to skip the instruction,
 * say); otherwise the exception is fatal.
 */
typedef bool (*sync_hook_t)(struct exc_frame *frame, uint64_t esr,
                            uint64_t far);
void exc_set_sync_hook(sync_hook_t hook);

/* ESR_ELx of the current exception level */
uint64_t exc_esr(void);
uint64_t exc_far(void);

#define ESR_EC(esr)             (((esr) >> 26) & 0x3f)
#define ESR_EC_HLT              0x32

#endif /* BM_EXCEPTION_H */
