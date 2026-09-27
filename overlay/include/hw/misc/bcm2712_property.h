/*
 * Raspberry Pi 5 firmware property interface
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_BCM2712_PROPERTY_H
#define HW_MISC_BCM2712_PROPERTY_H

#include "hw/misc/bcm2835_property.h"
#include "qom/object.h"

#define TYPE_BCM2712_PROPERTY "bcm2712-property"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2712PropertyState, BCM2712_PROPERTY)

/* The clocks the firmware lists in answer to GET_CLOCKS */
#define BCM2712_PROPERTY_NUM_CLOCKS     5

/* A reboot flag: the bootloader is to make the next boot a tryboot */
#define BCM2712_REBOOT_FLAG_TRYBOOT     (1u << 0)

/*
 * The property channel of bcm2835-property, answered as the Raspberry
 * Pi 5's firmware answers it. Two parts of it outlive a reset: the
 * @reboot_flags, which the board consumes for the boot the reset starts,
 * and the real-time clock, whose time is rtc_clock's plus @rtc_offset,
 * modulo 2^32 as its register counts.
 */
struct BCM2712PropertyState {
    /*< private >*/
    BCM2835PropertyState parent_obj;

    /*< public >*/
    uint32_t clock_rate[BCM2712_PROPERTY_NUM_CLOCKS];
    uint32_t clocks_on;         /* bit N: the Nth clock of GET_CLOCKS */
    uint32_t domains_on;        /* bit N: power domain N */
    uint32_t devices_on;        /* bit N: device N of GET_POWER_STATE */
    uint32_t reboot_flags;

    QEMUTimer *rtc_timer;       /* goes off at the enabled alarm */
    uint32_t rtc_offset;        /* seconds */
    uint32_t rtc_alarm;         /* seconds since 1970 */
    bool rtc_alarm_enabled;
    bool rtc_alarm_pending;
    uint32_t rtc_charge_uv;     /* the battery's charging voltage, or 0 */
};

#endif /* HW_MISC_BCM2712_PROPERTY_H */
