/*
 * Broadcom AVS monitor (BCM2711, BCM2712)
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_BCM2711_AVS_MONITOR_H
#define HW_MISC_BCM2711_AVS_MONITOR_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2711_AVS_MONITOR "bcm2711-avs-monitor"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2711AVSMonitorState, BCM2711_AVS_MONITOR)

#define BCM2711_AVS_MONITOR_MMIO_SIZE   0xf00

/*
 * The temperature sensor's code converts to millidegrees Celsius as
 * @slope * code + @offset, the coefficients of the chip's thermal zone in
 * its device tree; @temperature is the temperature the chip is at, in
 * millidegrees Celsius, of which the sensor reports the nearest code.
 */
struct BCM2711AVSMonitorState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    int32_t slope;
    int32_t offset;
    int32_t temperature;
};

/**
 * bcm2711_avs_monitor_get_temperature: the temperature the sensor reports
 * @s: the AVS monitor
 *
 * Return the temperature, in millidegrees Celsius, that the code the
 * sensor reports stands for: the one software converting it finds, within
 * half a step of the temperature the chip is at.
 */
int32_t bcm2711_avs_monitor_get_temperature(BCM2711AVSMonitorState *s);

#endif /* HW_MISC_BCM2711_AVS_MONITOR_H */
