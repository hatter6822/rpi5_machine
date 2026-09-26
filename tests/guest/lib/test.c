/*
 * Test runner: runs every TEST() and prints the transcript.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/console.h>
#include <bm/exception.h>
#include <bm/fdt.h>
#include <bm/io.h>
#include <bm/pm.h>
#include <bm/runtime.h>
#include <bm/string.h>
#include <bm/test.h>

#define MAX_TESTS               128
#define MAX_RESETS              8       /* per test, to stop a reset loop */

extern const struct bm_test __tests_start[], __tests_end[];

enum outcome { OUTCOME_PASS, OUTCOME_FAIL, OUTCOME_SKIP };

/*
 * The runner's progress, kept across system resets so that a test that
 * resets the machine is run again, and the run carries on after it.
 */
#define PROGRESS_MAGIC          0x73736572676f7270ull   /* "progress" */

struct progress {
    uint64_t magic;
    uint32_t next;              /* the running test */
    uint32_t resets;            /* resets it has survived */
    uint32_t passed, failed, skipped;
    uint64_t scratch[BM_TEST_SCRATCH_WORDS];
};

_Static_assert(sizeof(struct progress) <= BM_PERSIST_WORDS * 8,
               "see bm_persistent()");

static const struct bm_test *current;
static enum outcome outcome;
static struct progress *run;

unsigned bm_test_resets(void)
{
    return run->resets;
}

uint64_t *bm_test_scratch(void)
{
    return run->scratch;
}

static const char *basename(const char *path)
{
    const char *base = path;

    for (; *path; path++) {
        if (*path == '/') {
            base = path + 1;
        }
    }
    return base;
}

void bm_test_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    outcome = OUTCOME_FAIL;
    bm_printf("FAIL: %s: %s:%d: ", current->name, basename(file), line);
    va_start(ap, fmt);
    bm_vprintf(fmt, ap);
    va_end(ap);
    console_puts("\n");
}

void bm_test_skip(const char *fmt, ...)
{
    va_list ap;

    outcome = OUTCOME_SKIP;
    bm_printf("SKIP: %s: ", current->name);
    va_start(ap, fmt);
    bm_vprintf(fmt, ap);
    va_end(ap);
    console_puts("\n");
}

void bm_test_note(const char *fmt, ...)
{
    va_list ap;

    console_puts("# ");
    va_start(ap, fmt);
    bm_vprintf(fmt, ap);
    va_end(ap);
    console_puts("\n");
}

static void summary(void)
{
    bm_printf("# passed %u, failed %u, skipped %u\n", run->passed,
              run->failed, run->skipped);
    console_puts(run->failed ? "END: FAIL\n" : "END: PASS\n");
    run->magic = 0;
}

/* A panic (unexpected exception or interrupt) ends the current test */
static void report_panic(void)
{
    if (current) {
        bm_printf("FAIL: %s: panic\n", current->name);
        run->failed++;
    }
    summary();
}

static int name_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static const char *source(bool from_dt)
{
    return from_dt ? "dt" : "default";
}

static void describe_platform(void)
{
    const struct bm_platform *p = &bm_plat;

    bm_test_note("raspi5b bare-metal tests");
    if (p->has_dtb) {
        bm_test_note("EL%u, %u cores, device tree at 0x%lx (%u bytes)",
                     current_el(), p->num_cpus, (uint64_t)p->dtb,
                     fdt_size());
    } else {
        bm_test_note("EL%u, %u cores, no device tree", current_el(),
                     p->num_cpus);
    }
    bm_test_note("pm 0x%lx (%s), boot %u, reset status 0x%x",
                 (uint64_t)p->pm, source(p->pm_from_dt), bm_boot_count(),
                 pm_read(PM_RSTS));
    bm_test_note("uart 0x%lx (%s), gic 0x%lx/0x%lx (%s), systimer 0x%lx "
                 "intid %u-%u (%s)", (uint64_t)p->uart,
                 source(p->uart_from_dt), (uint64_t)p->gicd,
                 (uint64_t)p->gicc, source(p->gic_from_dt),
                 (uint64_t)p->systimer, p->systimer_intid[0],
                 p->systimer_intid[BM_SYSTIMER_COMPARATORS - 1],
                 source(p->systimer_from_dt));
}

int bm_main(void)
{
    const struct bm_test *tests[MAX_TESTS];
    unsigned n = 0;
    bool resumed;

    describe_platform();
    for (const struct bm_test *t = __tests_start; t < __tests_end; t++) {
        if (n == MAX_TESTS) {
            bm_panic("more than %u tests", MAX_TESTS);
        }
        /* Insertion sort by name, so the order is stable across builds */
        unsigned i = n++;

        for (; i > 0 && name_cmp(tests[i - 1]->name, t->name) > 0; i--) {
            tests[i] = tests[i - 1];
        }
        tests[i] = t;
    }

    run = (struct progress *)bm_persistent();
    resumed = run->magic == PROGRESS_MAGIC && run->next < n;
    if (resumed) {
        run->resets++;
        bm_test_note("boot %u: %s reset the machine, running it again",
                     bm_boot_count(), tests[run->next]->name);
    } else {
        memset(run, 0, sizeof(*run));
        run->magic = PROGRESS_MAGIC;
        bm_test_note("%u tests", n);
    }

    bm_panic_hook = report_panic;
    for (unsigned i = resumed ? run->next : 0; i < n; i++) {
        current = tests[i];
        outcome = OUTCOME_PASS;
        if (run->next != i || !resumed) {
            run->next = i;
            run->resets = 0;
            memset(run->scratch, 0, sizeof(run->scratch));
        }
        resumed = false;

        if (run->resets > MAX_RESETS) {
            bm_test_fail(__FILE__, __LINE__, "reset %u times", run->resets);
        } else {
            current->fn();
        }
        irq_mask();
        exc_set_sync_hook(NULL);

        switch (outcome) {
        case OUTCOME_PASS:
            bm_printf("PASS: %s\n", current->name);
            run->passed++;
            break;
        case OUTCOME_FAIL:
            run->failed++;
            break;
        case OUTCOME_SKIP:
            run->skipped++;
            break;
        }
    }
    current = NULL;
    summary();
    return run->failed ? 1 : 0;
}
