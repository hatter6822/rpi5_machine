/*
 * Entry points and exception vectors for the bare-metal test library.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Runs at whatever exception level the machine enters at (EL2 by default,
 * EL3 with secure=on) and stays there, with the MMU and caches off.
 */

#define STACK_SHIFT     14      /* 16 KiB per core, see link.ld */
#define FRAME_SIZE      (34 * 8)

/* Routing bits that make interrupts target the current exception level */
#define SCR_IRQ         (1 << 1)
#define SCR_FIQ         (1 << 2)
#define SCR_EA          (1 << 3)
#define HCR_FMO         (1 << 3)
#define HCR_IMO         (1 << 4)
#define HCR_AMO         (1 << 5)

    .section .text.boot, "ax"

/*
 * Reset entry, x0 = device tree for a Linux-style entry. With PSCI only
 * core 0 arrives here; when the guest owns EL3 (secure=on) every core
 * does, and all but core 0 park.
 */
    .global _start
_start:
    mov     x19, x0
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
    bl      el_setup
    mov     x0, x19
    bl      bm_start
    b       park

/* PSCI CPU_ON target (bm_start_core): x0 holds the core number */
    .global secondary_entry
secondary_entry:
    bl      set_stack
    mov     x19, x0
    bl      el_setup
    mov     x0, x19
    bl      bm_secondary_start
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

/*
 * Install the vectors at the current exception level and route physical
 * interrupts and SErrors to it, so they are taken without leaving it.
 */
el_setup:
    adr     x1, vectors
    mrs     x2, CurrentEL
    cmp     x2, #(3 << 2)
    b.eq    3f
    cmp     x2, #(2 << 2)
    b.eq    2f
    msr     vbar_el1, x1
    b       9f
2:  msr     vbar_el2, x1
    mrs     x2, hcr_el2
    orr     x2, x2, #(HCR_FMO | HCR_IMO | HCR_AMO)
    msr     hcr_el2, x2
    b       9f
3:  msr     vbar_el3, x1
    mrs     x2, scr_el3
    orr     x2, x2, #(SCR_IRQ | SCR_FIQ | SCR_EA)
    msr     scr_el3, x2
9:  isb
    ret

/*
 * Save the interrupted context (struct exc_frame), call
 * exception_dispatch(frame, vector) and return to it. The vector number
 * is (source << 2) | kind, as in enum exc_source and enum exc_kind.
 */
.macro ventry num
    .balign 0x80
    sub     sp, sp, #FRAME_SIZE
    stp     x0, x1, [sp, #0]
    mov     x1, #\num
    b       exception_entry
.endm

    .text
    .balign 0x800
vectors:
    .irp    num, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    ventry  \num
    .endr

exception_entry:
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]
    bl      read_elr_spsr           /* x2 = ELR, x3 = SPSR */
    stp     x2, x3, [sp, #248]
    mov     x0, sp
    bl      exception_dispatch
    ldp     x2, x3, [sp, #248]
    bl      write_elr_spsr
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    add     sp, sp, #FRAME_SIZE
    eret

/* Clobber only x2-x4 (x0/x1 are live in exception_entry) */
read_elr_spsr:
    mrs     x4, CurrentEL
    cmp     x4, #(3 << 2)
    b.eq    3f
    cmp     x4, #(2 << 2)
    b.eq    2f
    mrs     x2, elr_el1
    mrs     x3, spsr_el1
    ret
2:  mrs     x2, elr_el2
    mrs     x3, spsr_el2
    ret
3:  mrs     x2, elr_el3
    mrs     x3, spsr_el3
    ret

write_elr_spsr:
    mrs     x4, CurrentEL
    cmp     x4, #(3 << 2)
    b.eq    3f
    cmp     x4, #(2 << 2)
    b.eq    2f
    msr     elr_el1, x2
    msr     spsr_el1, x3
    ret
2:  msr     elr_el2, x2
    msr     spsr_el2, x3
    ret
3:  msr     elr_el3, x2
    msr     spsr_el3, x3
    ret
