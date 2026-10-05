/*
 * Broadcom AVS monitor (BCM2711, BCM2712)
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * No public datasheet. The adaptive voltage scaling (AVS) monitor measures
 * the chip's temperature, among other things; the one register modelled
 * is the one Linux's bcm2711_thermal driver reads, AVS_RO_TEMP_STATUS,
 * under the "brcm,bcm2711-avs-monitor" node that the Raspberry Pi 4's and
 * 5's device trees have. It holds a 10-bit code, which the tree's thermal
 * zone converts to millidegrees Celsius with its coefficients, as
 * slope * code + offset: -487 and 410040 on the BCM2711, -550 and 450000
 * on the BCM2712. The "slope" and "offset" properties take them; an SoC
 * that sets them, as the BCM2712 does, wins over -global.
 *
 * The chip is at the temperature the "temperature" property gives, in
 * millidegrees Celsius, 25 degrees C unless set, which qom-set changes at
 * run time; the sensor reports the code nearest to it. Every other
 * register reads as 0, and writes are ignored.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "hw/misc/bcm2711_avs_monitor.h"
#include "migration/vmstate.h"
#include "trace.h"

REG32(RO_TEMP_STATUS,           0x200)
    FIELD(RO_TEMP_STATUS, CODE,  0, 10)

/*
 * Linux takes the code as valid if either of these bits is set: the
 * model sets both. Unverified (PLAN.md P13): what the other bits read.
 */
#define RO_TEMP_STATUS_VALID    (BIT(16) | BIT(10))

#define CODE_MAX                R_RO_TEMP_STATUS_CODE_MASK

#define DEFAULT_TEMPERATURE     25000

/* The code nearest @temperature, which may lie outside the code's range */
static int64_t bcm2711_avs_monitor_code(BCM2711AVSMonitorState *s,
                                        int64_t temperature)
{
    int64_t num = temperature - s->offset;
    int64_t den = s->slope;

    if (den < 0) {
        num = -num;
        den = -den;
    }
    /* Round half away from zero, as division truncates towards it */
    return (num < 0 ? num - den / 2 : num + den / 2) / den;
}

/* The temperature code @code stands for */
static int64_t bcm2711_avs_monitor_code_temperature(BCM2711AVSMonitorState *s,
                                                    int64_t code)
{
    return s->offset + s->slope * code;
}

static bool bcm2711_avs_monitor_check(BCM2711AVSMonitorState *s,
                                      int64_t temperature, Error **errp)
{
    int64_t code = bcm2711_avs_monitor_code(s, temperature);

    if (code < 0 || code > CODE_MAX) {
        int64_t first = bcm2711_avs_monitor_code_temperature(s, 0);
        int64_t last = bcm2711_avs_monitor_code_temperature(s, CODE_MAX);

        error_setg(errp, "temperature %" PRId64 " is out of the sensor's "
                   "range, %" PRId64 " to %" PRId64 " millidegrees Celsius",
                   temperature, MIN(first, last), MAX(first, last));
        return false;
    }
    return true;
}

int32_t bcm2711_avs_monitor_get_temperature(BCM2711AVSMonitorState *s)
{
    return bcm2711_avs_monitor_code_temperature(s,
               bcm2711_avs_monitor_code(s, s->temperature));
}

static uint64_t bcm2711_avs_monitor_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    BCM2711AVSMonitorState *s = BCM2711_AVS_MONITOR(opaque);
    uint32_t value = 0;

    switch (offset) {
    case A_RO_TEMP_STATUS:
        value = FIELD_DP32(RO_TEMP_STATUS_VALID, RO_TEMP_STATUS, CODE,
                           bcm2711_avs_monitor_code(s, s->temperature));
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented register 0x%03"
                      HWADDR_PRIx "\n", __func__, offset);
        break;
    }

    trace_bcm2711_avs_monitor_read(offset, value);
    return value;
}

static void bcm2711_avs_monitor_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented write of 0x%08" PRIx64
                  " to register 0x%03" HWADDR_PRIx "\n",
                  __func__, value, offset);
}

static const MemoryRegionOps bcm2711_avs_monitor_ops = {
    .read = bcm2711_avs_monitor_read,
    .write = bcm2711_avs_monitor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void bcm2711_avs_monitor_get_temp(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    BCM2711AVSMonitorState *s = BCM2711_AVS_MONITOR(obj);
    int64_t value = s->temperature;

    visit_type_int(v, name, &value, errp);
}

static void bcm2711_avs_monitor_set_temp(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    BCM2711AVSMonitorState *s = BCM2711_AVS_MONITOR(obj);
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }
    if (value < INT32_MIN || value > INT32_MAX) {
        error_setg(errp, "temperature %" PRId64 " is out of range", value);
        return;
    }
    /* Until the device is realized, its coefficients may still change */
    if (DEVICE(obj)->realized && !bcm2711_avs_monitor_check(s, value, errp)) {
        return;
    }
    s->temperature = value;
    trace_bcm2711_avs_monitor_temperature(s->temperature);
}

static void bcm2711_avs_monitor_realize(DeviceState *dev, Error **errp)
{
    BCM2711AVSMonitorState *s = BCM2711_AVS_MONITOR(dev);
    int64_t first, last;

    /* Every code must stand for a temperature */
    first = bcm2711_avs_monitor_code_temperature(s, 0);
    last = bcm2711_avs_monitor_code_temperature(s, CODE_MAX);
    if (!s->slope || MIN(first, last) < INT32_MIN ||
        MAX(first, last) > INT32_MAX) {
        error_setg(errp, "%s: slope %" PRId32 " and offset %" PRId32
                   " do not convert every code", __func__, s->slope,
                   s->offset);
        return;
    }
    bcm2711_avs_monitor_check(s, s->temperature, errp);
}

static void bcm2711_avs_monitor_init(Object *obj)
{
    BCM2711AVSMonitorState *s = BCM2711_AVS_MONITOR(obj);

    s->temperature = DEFAULT_TEMPERATURE;
    memory_region_init_io(&s->iomem, obj, &bcm2711_avs_monitor_ops, s,
                          TYPE_BCM2711_AVS_MONITOR,
                          BCM2711_AVS_MONITOR_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

/* A temperature the sensor cannot report does not come in */
static int bcm2711_avs_monitor_post_load(void *opaque, int version_id)
{
    BCM2711AVSMonitorState *s = opaque;

    return bcm2711_avs_monitor_check(s, s->temperature, NULL) ? 0 : -EINVAL;
}

static const VMStateDescription vmstate_bcm2711_avs_monitor = {
    .name = TYPE_BCM2711_AVS_MONITOR,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = bcm2711_avs_monitor_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(temperature, BCM2711AVSMonitorState),
        VMSTATE_END_OF_LIST()
    }
};

/* The BCM2711's coefficients, from its device tree */
static const Property bcm2711_avs_monitor_properties[] = {
    DEFINE_PROP_INT32("slope", BCM2711AVSMonitorState, slope, -487),
    DEFINE_PROP_INT32("offset", BCM2711AVSMonitorState, offset, 410040),
};

static void bcm2711_avs_monitor_class_init(ObjectClass *klass,
                                           const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bcm2711_avs_monitor_realize;
    dc->vmsd = &vmstate_bcm2711_avs_monitor;
    dc->desc = "Broadcom AVS monitor";
    device_class_set_props(dc, bcm2711_avs_monitor_properties);
    object_class_property_add(klass, "temperature", "int",
                              bcm2711_avs_monitor_get_temp,
                              bcm2711_avs_monitor_set_temp, NULL, NULL);
    object_class_property_set_description(klass, "temperature",
        "The chip's temperature, in millidegrees Celsius");
}

static const TypeInfo bcm2711_avs_monitor_types[] = {
    {
        .name           = TYPE_BCM2711_AVS_MONITOR,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(BCM2711AVSMonitorState),
        .instance_init  = bcm2711_avs_monitor_init,
        .class_init     = bcm2711_avs_monitor_class_init,
    },
};

DEFINE_TYPES(bcm2711_avs_monitor_types)
