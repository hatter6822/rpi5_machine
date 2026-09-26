/*
 * Console output over a PL011 UART.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef BM_CONSOLE_H
#define BM_CONSOLE_H

#include <stdarg.h>
#include <stdint.h>

void console_init(uintptr_t pl011_base);
void console_putc(char c);
void console_puts(const char *s);

/*
 * A printf subset: %c %s %d %i %u %x %p %%, the 0 and - flags, a field
 * width, and the l, ll and z length modifiers. '\n' is sent as "\r\n".
 * Only core 0 may print: with the MMU off there is no lock to share the
 * UART between cores.
 */
void bm_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void bm_vprintf(const char *fmt, va_list ap);

#endif /* BM_CONSOLE_H */
