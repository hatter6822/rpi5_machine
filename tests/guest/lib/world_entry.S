/*
 * Running code in the Non-secure world, for a guest that owns EL3.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * world_enter() saves its caller's callee-saved registers and stack
 * pointer, then drops to @fn at the exception level and stack its
 * arguments give. When @fn returns, the trampoline issues
 * SMC #BM_WORLD_RETURN; the EL3 exception handler calls world_resume(),
 * which restores the saved context and returns from world_enter() on the
 * caller's behalf. Only core 0 may use it: there is one saved context.
 */

#include <bm/world.h>

    .text

/* void world_enter(void (*fn)(void), uintptr_t sp_el2, uint64_t spsr) */
    .global world_enter
world_enter:
    ldr     x9, =world_ctx
    stp     x19, x20, [x9, #0]
    stp     x21, x22, [x9, #16]
    stp     x23, x24, [x9, #32]
    stp     x25, x26, [x9, #48]
    stp     x27, x28, [x9, #64]
    stp     x29, x30, [x9, #80]
    mov     x10, sp
    str     x10, [x9, #96]
    msr     sp_el2, x1
    msr     spsr_el3, x2
    adr     x3, world_trampoline
    msr     elr_el3, x3
    mov     x19, x0
    isb
    eret

world_trampoline:
    blr     x19
    smc     #BM_WORLD_RETURN
    b       .

/* void world_resume(void), at EL3 */
    .global world_resume
world_resume:
    ldr     x9, =world_ctx
    ldp     x19, x20, [x9, #0]
    ldp     x21, x22, [x9, #16]
    ldp     x23, x24, [x9, #32]
    ldp     x25, x26, [x9, #48]
    ldp     x27, x28, [x9, #64]
    ldp     x29, x30, [x9, #80]
    ldr     x10, [x9, #96]
    mov     sp, x10
    ret

    .bss
    .balign 16
world_ctx:
    .skip   13 * 8
