/*
 * The RNG200 random number generator.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Registers as used by Linux drivers/char/hw_random/iproc-rng200.c.
 */

#ifndef BM_RNG_H
#define BM_RNG_H

#include <stdbool.h>
#include <stdint.h>

#define RNG_CTRL                0x00
#define RNG_CTRL_RBGEN_MASK     0x1fff
#define RNG_CTRL_RBGEN_ENABLE   0x1
#define RNG_CTRL_DIV_CTRL_SHIFT 13
#define RNG_SOFT_RESET          0x04
#define RBG_SOFT_RESET          0x08
#define RNG_TOTAL_BIT_COUNT     0x0c
#define RNG_TOTAL_BIT_COUNT_THRESHOLD 0x10
#define RNG_INT_STATUS          0x18
#define RNG_INT_STATUS_TOTAL_BITS_COUNT         0x00000001
#define RNG_INT_STATUS_NIST_FAIL                0x00000020
#define RNG_INT_STATUS_STARTUP_TRANSITIONS_MET  0x00020000
#define RNG_INT_STATUS_MASTER_FAIL_LOCKOUT      0x80000000
#define RNG_INT_ENABLE          0x1c
#define RNG_FIFO_DATA           0x20
#define RNG_FIFO_COUNT          0x24
#define RNG_FIFO_COUNT_MASK     0xff
#define RNG_FIFO_THRESHOLD_SHIFT 8

/* Bits discarded after start-up, as Linux's bcm2711_rng200_init() asks */
#define RNG_WARMUP_BITS         0x40000

uint32_t rng_read(uint32_t reg);
void rng_write(uint32_t reg, uint32_t val);

bool rng_running(void);

/* Start the generator as Linux does, unless it already runs */
void rng_start(void);
void rng_stop(void);

/*
 * Read up to @n words, waiting up to @wait_us for the warm-up to end and
 * for the FIFO to fill (bcm2711_rng200_read()). Returns the words read.
 */
unsigned rng_words(uint32_t *out, unsigned n, uint64_t wait_us);

#endif /* BM_RNG_H */
