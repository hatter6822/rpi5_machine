/*
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Linked at 2 MiB so that QEMU can place a -dtb blob at the base of RAM.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x200000;

    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }
    .rodata : ALIGN(8) { *(.rodata .rodata.*) }
    .tests : ALIGN(8) {
        __tests_start = .;
        KEEP(*(SORT(.tests.*)))
        __tests_end = .;
    }
    .data : ALIGN(8) { *(.data .data.*) }
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    . = ALIGN(16);
    . += 4 * 16K;               /* one stack per core, see start.S */
    __stack_top = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
