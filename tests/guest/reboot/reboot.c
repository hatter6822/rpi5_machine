/*
 * raspi5b bare-metal guest that reboots as its command line says.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * An OS for the reboots of the rpi5-boot tests (test_rpi5_boot.py): it
 * reports the boot the firmware says it is making, from
 * /chosen/bootloader, with the "tag=" word of its command line, and ends
 * boot N as the word "bootN=" says:
 *
 *   reboot         PSCI SYSTEM_RESET, as Linux reboots on QEMU's PSCI
 *   tryboot        the same, with the firmware's tryboot flag set first,
 *                  as Linux's reboot "0 tryboot" sets it
 *   partitionP     a watchdog reset with partition P in the reset status,
 *                  as Linux's watchdog driver reboots to a partition
 *   halt           the same with partition 63, as Linux's watchdog driver
 *                  powers off
 *   off, or none   PSCI SYSTEM_OFF
 */

#include <stdbool.h>
#include <stddef.h>

#include <bm/console.h>
#include <bm/fdt.h>
#include <bm/mbox.h>
#include <bm/pm.h>
#include <bm/psci.h>
#include <bm/runtime.h>
#include <bm/timer.h>

/* Beyond this boot the count is not what the steps expect: stop */
#define LAST_BOOT       8

/* A word of the command line: where it starts, and its length */
struct word {
    const char *s;
    size_t len;
};

/* The word after @w in @args, or one of length 0 at the end */
static struct word next_word(const char *args, struct word w)
{
    const char *p = w.s ? w.s + w.len : args;

    while (*p == ' ') {
        p++;
    }
    w.s = p;
    for (w.len = 0; p[w.len] && p[w.len] != ' '; w.len++) {
        continue;
    }
    return w;
}

/* Whether @w starts with @prefix; if so, @rest is what follows it */
static bool starts(struct word w, const char *prefix, struct word *rest)
{
    size_t i = 0;

    for (; prefix[i]; i++) {
        if (i >= w.len || w.s[i] != prefix[i]) {
            return false;
        }
    }
    rest->s = w.s + i;
    rest->len = w.len - i;
    return true;
}

/*
 * The decimal number at the start of @w, and in @rest what follows it;
 * false if there is none
 */
static bool number(struct word w, unsigned *value, struct word *rest)
{
    size_t i = 0;

    for (*value = 0; i < w.len && w.s[i] >= '0' && w.s[i] <= '9'; i++) {
        *value = *value * 10 + (w.s[i] - '0');
    }
    rest->s = w.s + i;
    rest->len = w.len - i;
    return i > 0;
}

static bool equals(struct word w, const char *text)
{
    struct word rest;

    return starts(w, text, &rest) && rest.len == 0;
}

/* The value of the word "@key=" of @args, or one of length 0 */
static struct word value_of(const char *args, const char *key)
{
    struct word w = { NULL, 0 }, rest;

    for (w = next_word(args, w); w.len; w = next_word(args, w)) {
        if (starts(w, key, &rest) && starts(rest, "=", &rest)) {
            return rest;
        }
    }
    return (struct word){ w.s, 0 };
}

/* The step of the word "boot@boot=", or one of length 0 */
static struct word step_of(const char *args, unsigned boot)
{
    struct word w = { NULL, 0 }, rest;
    unsigned n;

    for (w = next_word(args, w); w.len; w = next_word(args, w)) {
        if (starts(w, "boot", &rest) && number(rest, &n, &rest) &&
            n == boot && starts(rest, "=", &rest)) {
            return rest;
        }
    }
    return (struct word){ w.s, 0 };
}

static void put_word(struct word w, const char *none)
{
    if (!w.len) {
        console_puts(none);
    }
    for (size_t i = 0; i < w.len; i++) {
        console_putc(w.s[i]);
    }
}

/* A reset the watchdog makes, with @partition in the reset status */
static void __attribute__((noreturn)) watchdog_reset(unsigned partition)
{
    pm_set_partition(partition);
    pm_watchdog_start(10);
    wait_until(false, 1000000);
    bm_panic("no reset a second after arming the watchdog");
}

int bm_main(void)
{
    int chosen = fdt_path_offset("/chosen");
    int bootloader = fdt_path_offset("/chosen/bootloader");
    uint32_t count, partition, tryboot, rsts, flags;
    const char *args = NULL;
    struct word tag, step, rest;
    unsigned target;

    if (chosen >= 0) {
        args = fdt_getprop(chosen, "bootargs", NULL);
    }
    if (bootloader < 0 || !args ||
        !fdt_prop_u32(bootloader, "count", &count) ||
        !fdt_prop_u32(bootloader, "partition", &partition) ||
        !fdt_prop_u32(bootloader, "tryboot", &tryboot) ||
        !fdt_prop_u32(bootloader, "rsts", &rsts)) {
        bm_printf("reboot: the device tree does not say which boot this "
                  "is\n");
        return 1;
    }
    tag = value_of(args, "tag");
    step = step_of(args, count);
    bm_printf("reboot: boot %u from partition %u, reset status 0x%x, "
              "tryboot %u, tag ", count, partition, rsts, tryboot);
    put_word(tag, "(none)");
    console_puts(": ");
    put_word(step, "off");
    console_puts("\n");
    if (count > LAST_BOOT) {
        bm_printf("reboot: too many boots\n");
        return 1;
    }

    if (equals(step, "tryboot")) {
        /* Linux's rpi_firmware_notify_reboot() */
        flags = 1;
        if (mbox_tag(FW_TAG_SET_REBOOT_FLAGS, &flags, 1) != 4) {
            bm_panic("SET_REBOOT_FLAGS failed");
        }
    }
    if (equals(step, "tryboot") || equals(step, "reboot")) {
        mbox_tag(FW_TAG_NOTIFY_REBOOT, NULL, 0);
        psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);
        bm_panic("SYSTEM_RESET returned");
    }
    if (starts(step, "partition", &rest) && number(rest, &target, &rest) &&
        rest.len == 0) {
        mbox_tag(FW_TAG_NOTIFY_REBOOT, NULL, 0);
        watchdog_reset(target);
    }
    if (equals(step, "halt")) {
        mbox_tag(FW_TAG_NOTIFY_REBOOT, NULL, 0);
        watchdog_reset(63);
    }
    if (step.len && !equals(step, "off")) {
        console_puts("reboot: no step ");
        put_word(step, "");
        console_puts("\n");
        return 1;
    }
    return 0;
}
