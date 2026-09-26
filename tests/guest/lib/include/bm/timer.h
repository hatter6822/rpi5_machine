/*
 * The Arm generic timer: counter reads, delays and timeouts.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef BM_TIMER_H
#define BM_TIMER_H

#include <bm/io.h>

static inline uint64_t counter_freq(void)
{
    return read_sysreg(cntfrq_el0);
}

/* Physical count, ordered after preceding instructions */
static inline uint64_t counter_now(void)
{
    isb();
    return read_sysreg(cntpct_el0);
}

static inline uint64_t us_to_ticks(uint64_t us)
{
    return us * counter_freq() / 1000000;
}

static inline void delay_us(uint64_t us)
{
    uint64_t end = counter_now() + us_to_ticks(us);

    while (counter_now() < end) {
        cpu_relax();
    }
}

/* Deadline @us microseconds from now, for timeout_expired() */
static inline uint64_t timeout_us(uint64_t us)
{
    return counter_now() + us_to_ticks(us);
}

static inline bool timeout_expired(uint64_t deadline)
{
    return counter_now() >= deadline;
}

/*
 * Spin until @cond holds or @us microseconds pass; evaluates to whether
 * @cond held. Volatile objects in @cond are re-read on every iteration.
 */
#define wait_until(cond, us) ({                             \
    uint64_t _deadline = timeout_us(us);                    \
    bool _ok;                                               \
    while (!(_ok = (cond)) && !timeout_expired(_deadline)) {\
        cpu_relax();                                        \
    }                                                       \
    _ok;                                                    \
})

#endif /* BM_TIMER_H */
