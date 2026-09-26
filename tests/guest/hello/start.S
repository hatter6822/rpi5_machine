/*
 * Entry points for the raspi5b bare-metal smoke test.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define STACK_SHIFT     14      /* 16 KiB per core */

    .section .text.boot, "ax"

/*
 * Reset entry. With emulated PSCI only core 0 arrives here; when the guest
 * owns EL3 (secure=on) every core does, and all but core 0 park.
 */
    .global _start
_start:
    mrs     x0, mpidr_el1
    ubfx    x0, x0, #8, #8          /* Aff1 is the core number */
    cbnz    x0, park

    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
1:  cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b

2:  bl      set_stack
    bl      primary_main
    b       park

/* PSCI CPU_ON target: x0 holds the context id, which is the core number */
    .global secondary_entry
secondary_entry:
    bl      set_stack
    bl      secondary_main
    /* fall through */

park:
    wfe
    b       park

/* sp = __stack_top - (core << STACK_SHIFT); core number in x0, preserved */
set_stack:
    ldr     x1, =__stack_top
    sub     x1, x1, x0, lsl #STACK_SHIFT
    mov     sp, x1
    ret
