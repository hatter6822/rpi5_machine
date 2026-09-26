/*
 * A minimal test framework with a line-based transcript.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 *   TEST(gic_sgi_self, "gic/sgi-self")
 *   {
 *       ASSERT_EQ(gic_num_irqs(), 320);
 *   }
 *
 * Tests run on core 0, one after the other, in name order. A test ends at
 * its first failed assertion; the transcript format is documented in
 * tests/guest/README.md.
 *
 * A test may reset the machine (the watchdog, PSCI SYSTEM_RESET). The
 * program then starts again, and the runner runs that test again before
 * carrying on with the next:
 *
 *   TEST(pm_watchdog_reset, "pm/watchdog-reset")
 *   {
 *       if (bm_test_resets() == 0) {
 *           pm_watchdog_start(10);
 *           ...wait, and fail if the reset does not come
 *       }
 *       ...check what the reset left behind
 *   }
 */

#ifndef BM_TEST_H
#define BM_TEST_H

#include <stdint.h>

struct bm_test {
    const char *name;
    void (*fn)(void);
};

#define TEST(id, name_)                                                 \
    static void test_##id(void);                                        \
    static const struct bm_test bm_test_##id                            \
        __attribute__((used, section(".tests." #id))) = {              \
        .name = name_, .fn = test_##id,                                 \
    };                                                                  \
    static void test_##id(void)

void bm_test_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void bm_test_skip(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* How many system resets the running test has caused: 0 on its first run */
unsigned bm_test_resets(void);

/* Words the running test keeps across its resets, zero on its first run */
#define BM_TEST_SCRATCH_WORDS   8
uint64_t *bm_test_scratch(void);

/* Informational line in the transcript ("# ..."), core 0 only */
void bm_test_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define ASSERT(cond) do {                                               \
    if (!(cond)) {                                                      \
        bm_test_fail(__FILE__, __LINE__, "%s", #cond);                  \
        return;                                                         \
    }                                                                   \
} while (0)

#define ASSERT_MSG(cond, ...) do {                                      \
    if (!(cond)) {                                                      \
        bm_test_fail(__FILE__, __LINE__, __VA_ARGS__);                  \
        return;                                                         \
    }                                                                   \
} while (0)

#define ASSERT_OP(a, op, b) do {                                        \
    uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b);                    \
    if (!(_a op _b)) {                                                  \
        bm_test_fail(__FILE__, __LINE__, "%s %s %s (0x%lx vs 0x%lx)",   \
                     #a, #op, #b, _a, _b);                              \
        return;                                                         \
    }                                                                   \
} while (0)

#define ASSERT_EQ(a, b)         ASSERT_OP(a, ==, b)
#define ASSERT_NE(a, b)         ASSERT_OP(a, !=, b)
#define ASSERT_LE(a, b)         ASSERT_OP(a, <=, b)
#define ASSERT_GE(a, b)         ASSERT_OP(a, >=, b)

#define SKIP(...) do {                                                  \
    bm_test_skip(__VA_ARGS__);                                          \
    return;                                                             \
} while (0)

#endif /* BM_TEST_H */
