/*
 * Raspberry Pi 5 firmware property interface
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The VideoCore firmware of the BCM2712 answers the property interface of
 * the older Raspberry Pis (bcm2835_property.c) with the Raspberry Pi 5's
 * own values where they differ, and keeps the state a guest sets through
 * it:
 *
 * - the clocks GET_CLOCKS lists, which Linux's raspberrypi-clk driver
 *   registers and raspberrypi-cpufreq scales the CPUs with: each clock's
 *   range, and the rate and on/off state a guest sets. No other clock
 *   exists.
 * - the power domains of the newer power interface (GET/SET_DOMAIN_STATE),
 *   through which Linux's raspberrypi-power driver switches every domain
 *   but USB, and the devices of the older one (GET/SET_POWER_STATE),
 *   through which it switches USB. Their states are storage: no modelled
 *   device depends on them.
 * - the reboot flags a guest sets before it reboots, of which bit 0 asks
 *   the bootloader for a tryboot. They outlive the reset, for the board
 *   to report the tryboot in the device tree of the boot it starts.
 * - the temperature limit, 85 degrees C, config.txt's temp_limit.
 *
 * A request for a clock or device that does not exist gets a rate of 0,
 * or state bit 1 set, as the firmware answers; one for a power domain
 * that does not exist gets the state off.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "hw/misc/bcm2712_property.h"
#include "hw/arm/raspberrypi-fw-defs.h"
#include "migration/vmstate.h"
#include "trace.h"

/*
 * The clocks, at the Raspberry Pi 5's defaults for config.txt's arm_freq,
 * core_freq, v3d_freq, isp_freq and hevc_freq (the most) and their _min
 * settings (the least), in MHz; the firmware starts each at its most.
 * They are the clocks the Pi 5's device trees take from the firmware and
 * whose rates the Raspberry Pi documentation gives, which leaves out the
 * display's: M2MC, PIXEL_BVB and DISP.
 * TODO(WS0.4): read GET_CLOCKS and the rates on hardware.
 */
static const struct {
    uint32_t id;
    uint32_t min_mhz;
    uint32_t max_mhz;
} bcm2712_property_clocks[BCM2712_PROPERTY_NUM_CLOCKS] = {
    { RPI_FIRMWARE_ARM_CLK_ID,  1500, 2400 },
    { RPI_FIRMWARE_CORE_CLK_ID,  500,  910 },
    { RPI_FIRMWARE_V3D_CLK_ID,   500,  960 },
    { RPI_FIRMWARE_ISP_CLK_ID,   500,  910 },
    { RPI_FIRMWARE_HEVC_CLK_ID,  500,  910 },
};

#define MHZ_TO_HZ(mhz)          ((mhz) * 1000000u)

/* The state word of the clock and power device tags */
#define STATE_ON                BIT(0)
#define STATE_NO_DEVICE         BIT(1)  /* in an answer */

/*
 * The power domains, numbered as Linux's raspberrypi-power binding numbers
 * them plus one: 1 (I2C0) to 23 (ARM)
 */
#define DOMAIN_ARM              23
#define DOMAIN_LAST             DOMAIN_ARM

/*
 * The devices of the older power interface: the SD card, UART0, UART1,
 * USB, I2C0 to I2C2, SPI and CCP2TX
 */
#define NUM_POWER_DEVICES       9

/* GET_MAX_TEMPERATURE, in thousandths of a degree C */
#define TEMP_LIMIT              85000

/* The index of clock @id in bcm2712_property_clocks, or -1 */
static int bcm2712_property_clock(uint32_t id)
{
    for (int i = 0; i < ARRAY_SIZE(bcm2712_property_clocks); i++) {
        if (bcm2712_property_clocks[i].id == id) {
            return i;
        }
    }
    return -1;
}

static uint32_t bcm2712_property_clock_state(BCM2712PropertyState *s,
                                             int clk)
{
    if (clk < 0) {
        return STATE_NO_DEVICE;
    }
    return extract32(s->clocks_on, clk, 1) ? STATE_ON : 0;
}

static uint32_t bcm2712_property_device_state(BCM2712PropertyState *s,
                                              uint32_t device)
{
    if (device >= NUM_POWER_DEVICES) {
        return STATE_NO_DEVICE;
    }
    return extract32(s->devices_on, device, 1) ? STATE_ON : 0;
}

static bool bcm2712_property_answer_tag(BCM2835PropertyState *ps,
                                        uint32_t tag, hwaddr value,
                                        uint32_t bufsize, size_t *resplen)
{
    BCM2712PropertyState *s = BCM2712_PROPERTY(ps);
    /* Most requests: an id, then a value */
    uint32_t id = bcm2835_property_get32(ps, value, bufsize, 0);
    uint32_t arg = bcm2835_property_get32(ps, value, bufsize, 4);
    int clk = bcm2712_property_clock(id);
    bool domain = id >= 1 && id <= DOMAIN_LAST;
    uint32_t answer;

    switch (tag) {
    case RPI_FWREQ_GET_CLOCKS:
        /* Each clock's parent, none, and id */
        for (int i = 0; i < ARRAY_SIZE(bcm2712_property_clocks); i++) {
            bcm2835_property_put32(ps, value, bufsize, 8 * i, 0);
            bcm2835_property_put32(ps, value, bufsize, 8 * i + 4,
                                   bcm2712_property_clocks[i].id);
        }
        *resplen = 8 * ARRAY_SIZE(bcm2712_property_clocks);
        return true;

    case RPI_FWREQ_SET_CLOCK_STATE:
        if (clk >= 0) {
            s->clocks_on = deposit32(s->clocks_on, clk, 1, arg & STATE_ON);
            trace_bcm2712_property_clock(id, s->clock_rate[clk],
                                         arg & STATE_ON);
        }
        /* fall through */
    case RPI_FWREQ_GET_CLOCK_STATE:
        answer = bcm2712_property_clock_state(s, clk);
        break;

    case RPI_FWREQ_SET_CLOCK_RATE:
        /* The firmware sets the rate nearest the one asked for */
        if (clk >= 0) {
            s->clock_rate[clk] =
                MAX(MIN(arg, MHZ_TO_HZ(bcm2712_property_clocks[clk].max_mhz)),
                    MHZ_TO_HZ(bcm2712_property_clocks[clk].min_mhz));
            trace_bcm2712_property_clock(id, s->clock_rate[clk],
                                         extract32(s->clocks_on, clk, 1));
        }
        /* fall through */
    case RPI_FWREQ_GET_CLOCK_RATE:
        /* The rate the clock runs at when on */
        answer = clk < 0 ? 0 : s->clock_rate[clk];
        break;

    case RPI_FWREQ_GET_CLOCK_MEASURED:
        answer = bcm2712_property_clock_state(s, clk) == STATE_ON ?
                 s->clock_rate[clk] : 0;
        break;

    case RPI_FWREQ_GET_MAX_CLOCK_RATE:
        answer = clk < 0 ? 0 :
                 MHZ_TO_HZ(bcm2712_property_clocks[clk].max_mhz);
        break;

    case RPI_FWREQ_GET_MIN_CLOCK_RATE:
        answer = clk < 0 ? 0 :
                 MHZ_TO_HZ(bcm2712_property_clocks[clk].min_mhz);
        break;

    case RPI_FWREQ_SET_POWER_STATE:
        if (id < NUM_POWER_DEVICES) {
            s->devices_on = deposit32(s->devices_on, id, 1, arg & STATE_ON);
            trace_bcm2712_property_device(id, arg & STATE_ON);
        }
        /* fall through */
    case RPI_FWREQ_GET_POWER_STATE:
        answer = bcm2712_property_device_state(s, id);
        break;

    case RPI_FWREQ_SET_DOMAIN_STATE:
        if (domain) {
            s->domains_on = deposit32(s->domains_on, id, 1, arg & 1);
            trace_bcm2712_property_domain(id, arg & 1);
        }
        /* fall through */
    case RPI_FWREQ_GET_DOMAIN_STATE:
        answer = domain && extract32(s->domains_on, id, 1);
        break;

    case RPI_FWREQ_GET_MAX_TEMPERATURE:
        answer = TEMP_LIMIT;
        break;

    case RPI_FWREQ_SET_REBOOT_FLAGS:
        s->reboot_flags = id;
        trace_bcm2712_property_reboot_flags(s->reboot_flags);
        /* fall through */
    case RPI_FWREQ_GET_REBOOT_FLAGS:
        *resplen = bcm2835_property_answer32(ps, value, bufsize,
                                             s->reboot_flags);
        return true;

    default:
        return false;
    }

    /* After the id, which stays as the request gave it */
    bcm2835_property_put32(ps, value, bufsize, 4, answer);
    *resplen = 8;
    return true;
}

/* The firmware's boot: every clock on at its most, and the ARM domain */
static void bcm2712_property_reset_enter(Object *obj, ResetType type)
{
    BCM2712PropertyState *s = BCM2712_PROPERTY(obj);

    for (int i = 0; i < ARRAY_SIZE(bcm2712_property_clocks); i++) {
        s->clock_rate[i] = MHZ_TO_HZ(bcm2712_property_clocks[i].max_mhz);
    }
    s->clocks_on = MAKE_64BIT_MASK(0, ARRAY_SIZE(bcm2712_property_clocks));
    s->domains_on = BIT(DOMAIN_ARM);
    s->devices_on = 0;
}

static const VMStateDescription vmstate_bcm2712_property = {
    .name = TYPE_BCM2712_PROPERTY,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parent_obj, BCM2712PropertyState, 0,
                       vmstate_bcm2835_property, BCM2835PropertyState),
        VMSTATE_UINT32_ARRAY(clock_rate, BCM2712PropertyState,
                             BCM2712_PROPERTY_NUM_CLOCKS),
        VMSTATE_UINT32(clocks_on, BCM2712PropertyState),
        VMSTATE_UINT32(domains_on, BCM2712PropertyState),
        VMSTATE_UINT32(devices_on, BCM2712PropertyState),
        VMSTATE_UINT32(reboot_flags, BCM2712PropertyState),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2712_property_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    BCM2835PropertyClass *bpc = BCM2835_PROPERTY_CLASS(klass);

    bpc->answer_tag = bcm2712_property_answer_tag;
    rc->phases.enter = bcm2712_property_reset_enter;
    dc->vmsd = &vmstate_bcm2712_property;
    dc->desc = "Raspberry Pi 5 firmware property interface";
}

static const TypeInfo bcm2712_property_types[] = {
    {
        .name           = TYPE_BCM2712_PROPERTY,
        .parent         = TYPE_BCM2835_PROPERTY,
        .instance_size  = sizeof(BCM2712PropertyState),
        .class_init     = bcm2712_property_class_init,
    },
};

DEFINE_TYPES(bcm2712_property_types)
