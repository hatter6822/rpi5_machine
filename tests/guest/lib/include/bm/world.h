/*
 * Running code in the Non-secure world, for a guest that owns EL3.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef BM_WORLD_H
#define BM_WORLD_H

/* The SMC immediate that ends a bm_run_nonsecure_el2() call */
#define BM_WORLD_RETURN         0x7e1

#ifndef __ASSEMBLER__

#include <stdint.h>

/* SCR_EL3 routing bits a caller may ask for: take these to EL3 */
#define SCR_IRQ                 (1u << 1)
#define SCR_FIQ                 (1u << 2)
#define SCR_EA                  (1u << 3)

/*
 * From EL3 on core 0, run @fn at Non-secure EL2 (AArch64, MMU off, the
 * library's vectors, @stack_top as its stack) and return once @fn does.
 * @route says which of IRQ, FIQ and SError are taken to EL3 meanwhile;
 * the others go to EL2. SCR_EL3 is restored afterwards, and interrupts
 * are masked.
 */
void bm_run_nonsecure_el2(void (*fn)(void), void *stack_top, uint32_t route);

/* Called by the EL3 exception handler on SMC #BM_WORLD_RETURN */
void __attribute__((noreturn)) world_resume(void);

#endif /* __ASSEMBLER__ */

#endif /* BM_WORLD_H */
