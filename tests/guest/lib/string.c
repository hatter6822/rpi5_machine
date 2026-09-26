/*
 * The memory functions the compiler may call even in freestanding code.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Byte at a time, so they are safe on Device memory (MMU off).
 */

#include <bm/string.h>

void *memset(void *s, int c, size_t n)
{
    volatile unsigned char *p = s;

    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

void *memmove(void *dst, const void *src, size_t n)
{
    volatile unsigned char *d = dst;
    const volatile unsigned char *s = src;

    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        while (n--) {
            d[n] = s[n];
        }
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    return memmove(dst, src, n);
}

int memcmp(const void *a, const void *b, size_t n)
{
    const volatile unsigned char *p = a, *q = b;

    for (; n--; p++, q++) {
        if (*p != *q) {
            return *p - *q;
        }
    }
    return 0;
}
