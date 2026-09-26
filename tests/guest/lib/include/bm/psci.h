/*
 * PSCI calls over SMC (Arm DEN 0022), as provided by the Pi 5's TF-A BL31
 * and by QEMU when the guest does not own EL3.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef BM_PSCI_H
#define BM_PSCI_H

#include <stdint.h>

#define PSCI_VERSION            0x84000000u
#define PSCI_CPU_OFF            0x84000002u
#define PSCI_CPU_ON_64          0xc4000003u
#define PSCI_AFFINITY_INFO_64   0xc4000004u
#define PSCI_SYSTEM_OFF         0x84000008u
#define PSCI_SYSTEM_RESET       0x84000009u

#define PSCI_SUCCESS            0
#define PSCI_NOT_SUPPORTED      (-1)
#define PSCI_INVALID_PARAMS     (-2)
#define PSCI_DENIED             (-3)
#define PSCI_ALREADY_ON         (-4)

int64_t psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3);

static inline int64_t psci_cpu_on(uint64_t mpidr, uintptr_t entry,
                                  uint64_t context)
{
    return psci_call(PSCI_CPU_ON_64, mpidr, entry, context);
}

#endif /* BM_PSCI_H */
