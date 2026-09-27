/*
 * AVS monitor temperature sensor tests (WS2.6).
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The SoC's temperature, read as Linux's bcm2711_thermal driver reads it,
 * converted with its thermal zone's coefficients, and as the firmware
 * reports it to vcgencmd measure_temp.
 */

#include <bm/fdt.h>
#include <bm/io.h>
#include <bm/mbox.h>
#include <bm/test.h>

/* Without a device tree: the AVS monitor in both raspi5b trees */
#define DEFAULT_AVS             0x107d542000ull

/*
 * The conversion the Pi 5's trees give the sensor's thermal zone, in
 * millidegrees Celsius: slope * code + offset
 */
#define DEFAULT_SLOPE           -550
#define DEFAULT_OFFSET          450000

/* Linux drivers/thermal/broadcom/bcm2711_thermal.c */
#define AVS_RO_TEMP_STATUS      0x200
#define AVS_TEMP_VALID          (BIT(16) | BIT(10))
#define AVS_TEMP_CODE           0x3ff

/* Where Linux shuts down: the trees' critical trip, 110 degrees C */
#define TEMP_CRITICAL           110000

struct avs {
    uintptr_t base;
    int32_t slope, offset;
    const char *source;
};

/* The thermal zone of the sensor node @sensor: its coefficients */
static bool find_zone(int sensor, int32_t *slope, int32_t *offset)
{
    int zones = fdt_path_offset("/thermal-zones");
    uint32_t phandle;

    if (zones < 0 || !fdt_prop_u32(sensor, "phandle", &phandle)) {
        return false;
    }
    for (int zone = fdt_first_subnode(zones); zone >= 0;
         zone = fdt_next_subnode(zone)) {
        uint32_t len, coeff_len;
        const void *sensors = fdt_getprop(zone, "thermal-sensors", &len);
        const void *coeff = fdt_getprop(zone, "coefficients", &coeff_len);

        if (sensors && len == 4 && fdt_cell(sensors, 0) == phandle &&
            coeff && coeff_len == 8) {
            *slope = (int32_t)fdt_cell(coeff, 0);
            *offset = (int32_t)fdt_cell(coeff, 1);
            return true;
        }
    }
    return false;
}

/*
 * The AVS monitor, and the conversion of its thermal sensor's zone, as
 * Linux finds them: an enabled "brcm,bcm2711-avs-monitor" node, its
 * "brcm,bcm2711-thermal" child, the zone whose sensor that is
 */
static bool find_avs(struct avs *avs)
{
    uint64_t addr, size;
    int node, sensor;

    if (!fdt_present()) {
        avs->base = DEFAULT_AVS;
        avs->slope = DEFAULT_SLOPE;
        avs->offset = DEFAULT_OFFSET;
        avs->source = "default";
        return true;
    }
    node = fdt_find_compatible("brcm,bcm2711-avs-monitor");
    if (node < 0 || !fdt_node_is_enabled(node) ||
        !fdt_reg(node, 0, &addr, &size)) {
        return false;
    }
    for (sensor = fdt_first_subnode(node); sensor >= 0;
         sensor = fdt_next_subnode(sensor)) {
        if (fdt_node_is_compatible(sensor, "brcm,bcm2711-thermal") &&
            fdt_node_is_enabled(sensor) &&
            find_zone(sensor, &avs->slope, &avs->offset)) {
            avs->base = addr;
            avs->source = "dt";
            return true;
        }
    }
    return false;
}

/*
 * The sensor reads valid, as Linux requires, and a temperature at which
 * the board runs, below the critical trip; the firmware reports the same
 * temperature, read from the same sensor, within 2 degrees (TODO(WS0.4):
 * check how closely it follows the sensor on hardware).
 */
TEST(avs_temperature, "avs/temperature")
{
    struct avs avs;
    uint32_t status, code, val[2] = { 0, 0 };
    int32_t temp, fw;

    if (!find_avs(&avs)) {
        SKIP("no AVS monitor thermal zone in the device tree");
    }
    status = mmio_read32(avs.base + AVS_RO_TEMP_STATUS);
    ASSERT_MSG(status & AVS_TEMP_VALID, "status 0x%x: no valid reading",
               status);
    code = status & AVS_TEMP_CODE;
    temp = avs.slope * (int32_t)code + avs.offset;

    ASSERT_EQ(mbox_tag(FW_TAG_TEMPERATURE, val, 2), 8);
    ASSERT_EQ(val[0], 0);                       /* the SoC's sensor */
    fw = (int32_t)val[1];
    bm_test_note("avs/temperature: monitor 0x%lx (%s), code %u, %d "
                 "millidegrees C, firmware %d", avs.base, avs.source, code,
                 temp, fw);

    ASSERT_MSG(temp > 0 && temp < TEMP_CRITICAL,
               "%d millidegrees C", temp);
    ASSERT_MSG(fw - temp <= 2000 && temp - fw <= 2000,
               "firmware %d, sensor %d millidegrees C", fw, temp);
}
