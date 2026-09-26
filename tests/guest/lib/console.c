/*
 * Console output over a PL011 UART, and a small printf.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/console.h>
#include <bm/io.h>

#define UART_DR                 0x00
#define UART_FR                 0x18
#define UART_IBRD               0x24
#define UART_FBRD               0x28
#define UART_LCRH               0x2c
#define UART_CR                 0x30
#define UART_FR_BUSY            BIT(3)
#define UART_FR_TXFF            BIT(5)
#define UART_LCRH_FEN           BIT(4)
#define UART_LCRH_WLEN_8        (3u << 5)
#define UART_CR_UARTEN          BIT(0)
#define UART_CR_TXE             BIT(8)
#define UART_CR_RXE             BIT(9)

/*
 * UART10 runs from a 9.216 MHz reference on the Pi 5 (the "uart10" clock
 * in bcm2712-rpi-5-b.dts); QEMU's PL011 ignores the divisor.
 */
#define UART_CLOCK_HZ           9216000u
#define UART_BAUD               115200u

static uintptr_t uart_base;

void console_init(uintptr_t pl011_base)
{
    uint32_t div64 = (4 * UART_CLOCK_HZ + UART_BAUD / 2) / UART_BAUD;

    uart_base = pl011_base;
    mmio_write32(uart_base + UART_CR, 0);
    while (mmio_read32(uart_base + UART_FR) & UART_FR_BUSY) {
    }
    mmio_write32(uart_base + UART_IBRD, div64 >> 6);
    mmio_write32(uart_base + UART_FBRD, div64 & 0x3f);
    mmio_write32(uart_base + UART_LCRH, UART_LCRH_WLEN_8 | UART_LCRH_FEN);
    mmio_write32(uart_base + UART_CR,
                 UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);
}

static void uart_putc(char c)
{
    if (!uart_base) {
        return;
    }
    while (mmio_read32(uart_base + UART_FR) & UART_FR_TXFF) {
    }
    mmio_write32(uart_base + UART_DR, (uint8_t)c);
}

void console_putc(char c)
{
    if (c == '\n') {
        uart_putc('\r');
    }
    uart_putc(c);
}

void console_puts(const char *s)
{
    for (; *s; s++) {
        console_putc(*s);
    }
}

static void put_padded(const char *s, size_t len, unsigned width, char pad,
                       bool left)
{
    for (; !left && width > len; width--) {
        console_putc(pad);
    }
    for (size_t i = 0; i < len; i++) {
        console_putc(s[i]);
    }
    for (; left && width > len; width--) {
        console_putc(' ');
    }
}

static void put_number(uint64_t v, unsigned base, bool negative,
                       unsigned width, char pad, bool left)
{
    char buf[24];
    size_t i = sizeof(buf);

    do {
        buf[--i] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    if (negative) {
        if (pad == '0' && width) {
            console_putc('-');
            width--;
        } else {
            buf[--i] = '-';
        }
    }
    put_padded(&buf[i], sizeof(buf) - i, width, pad, left);
}

void bm_vprintf(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        unsigned width = 0, longs = 0;
        bool left = false;
        char pad = ' ';
        uint64_t u;
        int64_t d;

        if (*fmt != '%') {
            console_putc(*fmt);
            continue;
        }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '0') {
                pad = '0';
            } else if (*fmt == '-') {
                left = true;
            } else {
                break;
            }
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        for (; *fmt == 'l' || *fmt == 'z'; fmt++) {
            longs += *fmt == 'z' ? 2 : 1;
        }

        switch (*fmt) {
        case 'c':
            console_putc((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            size_t len = 0;

            s = s ? s : "(null)";
            while (s[len]) {
                len++;
            }
            put_padded(s, len, width, ' ', left);
            break;
        }
        case 'd':
        case 'i':
            d = longs ? va_arg(ap, int64_t) : va_arg(ap, int);
            u = d < 0 ? -(uint64_t)d : (uint64_t)d;
            put_number(u, 10, d < 0, width, pad, left);
            break;
        case 'u':
        case 'x':
            u = longs ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            put_number(u, *fmt == 'u' ? 10 : 16, false, width, pad, left);
            break;
        case 'p':
            console_puts("0x");
            put_number((uintptr_t)va_arg(ap, void *), 16, false, 0, ' ',
                       false);
            break;
        case '%':
            console_putc('%');
            break;
        case '\0':
            return;
        default:
            console_putc('%');
            console_putc(*fmt);
            break;
        }
    }
}

void bm_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    bm_vprintf(fmt, ap);
    va_end(ap);
}
