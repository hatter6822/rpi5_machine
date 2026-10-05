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
 * - the temperature, which the firmware reads from the chip's AVS
 *   monitor, linked as "avs-monitor", as Linux's thermal driver does,
 *   and the temperature limit, 85 degrees C, config.txt's temp_limit.
 * - the real-time clock, which the firmware keeps in the power management
 *   IC and Linux's rtc-rpi driver reads and sets through GET/SET_RTC_REG:
 *   the time, which starts as QEMU's RTC (-rtc) and runs on through a
 *   reset, an alarm, and the backup battery's charger. The alarm powers a
 *   Pi 5 that is off back on; QEMU has no such state, and only reports
 *   the alarm pending.
 * - the text commands of GET_GENCMD_RESULT, through which vcgencmd asks,
 *   for the state above: see bcm2712_property_gencmd().
 *
 * A request for a clock or device that does not exist gets a rate of 0,
 * or state bit 1 set, as the firmware answers; one for a power domain
 * that does not exist gets the state off.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/misc/bcm2712_property.h"
#include "hw/arm/raspberrypi-fw-defs.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/qapi-events-misc.h"
#include "system/dma.h"
#include "system/rtc.h"
#include "system/system.h"
#include "trace.h"

/*
 * The clocks, at the Raspberry Pi 5's defaults for config.txt's arm_freq,
 * core_freq, v3d_freq, isp_freq and hevc_freq (the most) and their _min
 * settings (the least), in MHz; the firmware starts each at its most.
 * They are the clocks the Pi 5's device trees take from the firmware and
 * whose rates the Raspberry Pi documentation gives, which leaves out the
 * display's: M2MC, PIXEL_BVB and DISP.
 * Unverified (PLAN.md P09): GET_CLOCKS and the rates on hardware.
 */
static const struct {
    uint32_t id;
    const char *name;           /* in vcgencmd and config.txt */
    uint32_t min_mhz;
    uint32_t max_mhz;
} bcm2712_property_clocks[BCM2712_PROPERTY_NUM_CLOCKS] = {
    { RPI_FIRMWARE_ARM_CLK_ID,  "arm",  1500, 2400 },
    { RPI_FIRMWARE_CORE_CLK_ID, "core",  500,  910 },
    { RPI_FIRMWARE_V3D_CLK_ID,  "v3d",   500,  960 },
    { RPI_FIRMWARE_ISP_CLK_ID,  "isp",   500,  910 },
    { RPI_FIRMWARE_HEVC_CLK_ID, "hevc",  500,  910 },
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

/*
 * GET_GENCMD_RESULT: a command as text, from the second word of the value
 * buffer on; the firmware answers in its place, after an error code in the
 * first word. vcgencmd sends a 4 KiB buffer, the longest text it takes.
 */
#define RPI_FWREQ_GET_GENCMD_RESULT     0x00030080
#define GENCMD_MAX                      4096

/* The firmware's release time, as GET_FIRMWARE_REVISION answers it */
#define FIRMWARE_REVISION               346337

/* The real-time clock's registers, as Linux's rtc-rpi driver numbers them */
enum {
    RTC_TIME,                   /* seconds since 1970, UTC */
    RTC_ALARM,                  /* the time the alarm goes off */
    RTC_ALARM_PENDING,          /* bit 0: it went off; write 1 to clear */
    RTC_ALARM_ENABLE,           /* bit 0 */
    RTC_BBAT_CHG_VOLTS,         /* the battery's charging voltage; 0: off */
    RTC_BBAT_CHG_VOLTS_MIN,
    RTC_BBAT_CHG_VOLTS_MAX,
    RTC_BBAT_VOLTS,             /* the battery's voltage */
};

/*
 * The charging voltages the Raspberry Pi documentation gives for the
 * battery, in microvolts. No battery is fitted: it reads 0 V.
 * Unverified (PLAN.md P10): the registers on hardware without a battery.
 */
#define RTC_CHARGE_MIN_UV       1300000
#define RTC_CHARGE_MAX_UV       4400000

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

static uint32_t bcm2712_property_rtc_time(BCM2712PropertyState *s)
{
    return s->rtc_offset +
           qemu_clock_get_ns(rtc_clock) / NANOSECONDS_PER_SECOND;
}

static void bcm2712_property_rtc_alarm(void *opaque)
{
    BCM2712PropertyState *s = opaque;

    s->rtc_alarm_pending = true;
    trace_bcm2712_property_rtc_alarm(s->rtc_alarm);
}

/*
 * An enabled alarm goes off as the time reaches it. The time counts
 * modulo 2^32, as its register does, so an alarm that the time has passed
 * is some 136 years away.
 */
static void bcm2712_property_rtc_schedule(BCM2712PropertyState *s)
{
    int64_t now = qemu_clock_get_ns(rtc_clock) / NANOSECONDS_PER_SECOND;
    uint32_t ticks = s->rtc_alarm - (uint32_t)(s->rtc_offset + now);

    if (!s->rtc_alarm_enabled) {
        timer_del(s->rtc_timer);
    } else if (ticks == 0) {
        timer_del(s->rtc_timer);
        bcm2712_property_rtc_alarm(s);
    } else {
        /* At the start of the alarm's second */
        timer_mod(s->rtc_timer, (now + ticks) * NANOSECONDS_PER_SECOND);
    }
}

static void bcm2712_property_rtc_set_time(BCM2712PropertyState *s,
                                          uint32_t value)
{
    g_autofree char *qom_path = object_get_canonical_path(OBJECT(s));
    time_t t = value;
    struct tm tm;

    s->rtc_offset += value - bcm2712_property_rtc_time(s);
    gmtime_r(&t, &tm);
    qapi_event_send_rtc_change(qemu_timedate_diff(&tm), qom_path);
    bcm2712_property_rtc_schedule(s);
}

/* Access real-time clock register @reg: write @value if @write, then read */
static uint32_t bcm2712_property_rtc(BCM2712PropertyState *s, uint32_t reg,
                                     bool write, uint32_t value)
{
    if (write) {
        trace_bcm2712_property_rtc(reg, value);
    }

    switch (reg) {
    case RTC_TIME:
        if (write) {
            bcm2712_property_rtc_set_time(s, value);
        }
        return bcm2712_property_rtc_time(s);

    case RTC_ALARM:
        if (write) {
            s->rtc_alarm = value;
            bcm2712_property_rtc_schedule(s);
        }
        return s->rtc_alarm;

    case RTC_ALARM_PENDING:
        if (write && (value & 1)) {
            s->rtc_alarm_pending = false;
        }
        return s->rtc_alarm_pending;

    case RTC_ALARM_ENABLE:
        if (write) {
            s->rtc_alarm_enabled = value & 1;
            bcm2712_property_rtc_schedule(s);
        }
        return s->rtc_alarm_enabled;

    case RTC_BBAT_CHG_VOLTS:
        /* Unverified (PLAN.md P11): a voltage out of range */
        if (write) {
            s->rtc_charge_uv = value ? MIN(MAX(value, RTC_CHARGE_MIN_UV),
                                           RTC_CHARGE_MAX_UV) : 0;
        }
        return s->rtc_charge_uv;

    case RTC_BBAT_CHG_VOLTS_MIN:
    case RTC_BBAT_CHG_VOLTS_MAX:
    case RTC_BBAT_VOLTS:
        if (write) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: real-time clock register %" PRIu32
                          " is read-only\n", __func__, reg);
        }
        return reg == RTC_BBAT_CHG_VOLTS_MIN ? RTC_CHARGE_MIN_UV :
               reg == RTC_BBAT_CHG_VOLTS_MAX ? RTC_CHARGE_MAX_UV : 0;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: no real-time clock register %" PRIu32 "\n",
                      __func__, reg);
        return 0;
    }
}

/* Append the setting @key=@value to the "get_config int" list @out */
static gboolean bcm2712_property_config_line(gpointer key, gpointer value,
                                             gpointer out)
{
    g_string_append_printf(out, "%s%s=%u", ((GString *)out)->len ? "\n" : "",
                           (char *)key, GPOINTER_TO_UINT(value));
    return false;
}

/*
 * The integer settings of config.txt that the machine decides, as
 * "get_config int" lists them (in order of name, without the ones that
 * are 0): each clock's range, the temperature limit and the memory size.
 * config.txt itself is the bootloader's, which QEMU does not run, so
 * every other setting reads as unset, 0.
 */
static void bcm2712_property_config(BCM2712PropertyState *s, GString *out,
                                    const char *name)
{
    BCM2835PropertyState *ps = BCM2835_PROPERTY(s);
    /* The board revision's memory size field: 256 MiB << n */
    uint32_t total_mem = 256 << extract32(ps->board_rev, 20, 3);
    g_autoptr(GTree) settings =
        g_tree_new_full((GCompareDataFunc)strcmp, NULL, g_free, NULL);

    for (int i = 0; i < ARRAY_SIZE(bcm2712_property_clocks); i++) {
        g_tree_insert(settings,
                      g_strdup_printf("%s_freq",
                                      bcm2712_property_clocks[i].name),
                      GUINT_TO_POINTER(bcm2712_property_clocks[i].max_mhz));
        g_tree_insert(settings,
                      g_strdup_printf("%s_freq_min",
                                      bcm2712_property_clocks[i].name),
                      GUINT_TO_POINTER(bcm2712_property_clocks[i].min_mhz));
    }
    g_tree_insert(settings, g_strdup("temp_limit"),
                  GUINT_TO_POINTER(TEMP_LIMIT / 1000));
    g_tree_insert(settings, g_strdup("total_mem"),
                  GUINT_TO_POINTER(total_mem));

    if (strcmp(name, "int") == 0) {
        g_tree_foreach(settings, bcm2712_property_config_line, out);
    } else if (strcmp(name, "str") != 0) {
        g_string_append_printf(out, "%s=%u", name,
                               GPOINTER_TO_UINT(g_tree_lookup(settings,
                                                              name)));
    }
}

/*
 * Answer the vcgencmd command @argv (@argc words) into @out; returns the
 * error code. The firmware answers the commands below as the Raspberry
 * Pi 5's does, in the forms its documentation and vcgencmd's show:
 *
 * - commands: the commands answered;
 * - measure_temp: the AVS monitor's temperature, as GET_TEMPERATURE;
 * - measure_clock NAME: a clock's rate as GET_CLOCK_MEASURED answers it.
 *   Any other name measures 0, as a clock that is off does: the firmware
 *   also measures the SD hosts', the UARTs' and the display's clocks,
 *   which it does not list and the model does not have;
 * - get_config NAME, get_config int, get_config str: the settings of
 *   bcm2712_property_config();
 * - get_throttled: no under-voltage and no throttling, which the model
 *   has neither of;
 * - version: the release and hash GET_FIRMWARE_REVISION and
 *   GET_FIRMWARE_HASH answer.
 *
 * Any other command is not registered, as the firmware answers a command
 * it lacks. That includes bootloader_version, bootloader_config and
 * otp_dump, which ask about the EEPROM bootloader QEMU does not have:
 * rpi-eeprom-update, which asks for the bootloader's version, then skips
 * the update it would otherwise offer.
 */
static int32_t bcm2712_property_gencmd(BCM2712PropertyState *s,
                                       GString *out, int argc, char **argv)
{
    const char *cmd = argc ? argv[0] : "";

    if (strcmp(cmd, "commands") == 0) {
        g_string_append(out, "commands=\"commands, measure_clock, "
                        "measure_temp, get_config, get_throttled, "
                        "version\"");
    } else if (strcmp(cmd, "measure_temp") == 0 && s->avs_monitor) {
        int32_t mc = bcm2711_avs_monitor_get_temperature(s->avs_monitor);
        /* To the nearest tenth of a degree */
        int32_t tenths = (mc + (mc < 0 ? -50 : 50)) / 100;

        g_string_append_printf(out, "temp=%s%d.%d'C",
                               tenths < 0 ? "-" : "",
                               abs(tenths) / 10, abs(tenths) % 10);
    } else if (strcmp(cmd, "measure_clock") == 0 && argc >= 2) {
        uint32_t hz = 0;

        for (int i = 0; i < ARRAY_SIZE(bcm2712_property_clocks); i++) {
            if (strcmp(argv[1], bcm2712_property_clocks[i].name) == 0) {
                hz = extract32(s->clocks_on, i, 1) ? s->clock_rate[i] : 0;
                break;
            }
        }
        /* The Pi 5's firmware numbers every clock 0 in its answer */
        g_string_append_printf(out, "frequency(0)=%u", hz);
    } else if (strcmp(cmd, "get_config") == 0 && argc >= 2) {
        bcm2712_property_config(s, out, argv[1]);
    } else if (strcmp(cmd, "get_throttled") == 0) {
        g_string_append(out, "throttled=0x0");
    } else if (strcmp(cmd, "version") == 0) {
        time_t t = FIRMWARE_REVISION;
        struct tm tm;
        char date[32];

        gmtime_r(&t, &tm);
        strftime(date, sizeof(date), "%Y/%m/%d %H:%M:%S", &tm);
        /* The hash's first four bytes, all zero as it is */
        g_string_append_printf(out, "%s\nCopyright (c) 2012 Broadcom\n"
                               "version 00000000 (release) (embedded)",
                               date);
    } else {
        qemu_log_mask(LOG_UNIMP, "%s: vcgencmd command '%s' not "
                      "implemented\n", __func__, cmd);
        g_string_append(out, "error=1 error_msg=\"Command not registered\"");
        return -1;
    }
    return 0;
}

/*
 * GET_GENCMD_RESULT: the command, split into words at spaces, and the
 * answer written in its place, cut to the buffer with its NUL kept.
 * Returns the response length, which is the whole answer's.
 */
static size_t bcm2712_property_gencmd_tag(BCM2712PropertyState *s,
                                          hwaddr value, uint32_t bufsize)
{
    BCM2835PropertyState *ps = BCM2835_PROPERTY(s);
    uint32_t textsize = bufsize < 4 ? 0 : MIN(bufsize - 4, GENCMD_MAX - 1);
    g_autofree char *text = g_malloc0(textsize + 1);
    g_autoptr(GString) out = g_string_new(NULL);
    g_autoptr(GPtrArray) argv = g_ptr_array_new();
    char *saveptr;
    int32_t err;

    dma_memory_read(&ps->dma_as, value + 12 + 4, text, textsize,
                    MEMTXATTRS_UNSPECIFIED);
    for (char *word = strtok_r(text, " ", &saveptr); word;
         word = strtok_r(NULL, " ", &saveptr)) {
        g_ptr_array_add(argv, word);
    }
    err = bcm2712_property_gencmd(s, out, argv->len, (char **)argv->pdata);
    trace_bcm2712_property_gencmd(argv->len ? (char *)argv->pdata[0] : "",
                                  err);

    bcm2835_property_put32(ps, value, bufsize, 0, err);
    if (textsize) {
        size_t len = MIN(out->len, textsize - 1);

        dma_memory_write(&ps->dma_as, value + 12 + 4, out->str, len,
                         MEMTXATTRS_UNSPECIFIED);
        address_space_set(&ps->dma_as, value + 12 + 4 + len, 0, 1,
                          MEMTXATTRS_UNSPECIFIED);
    }
    return 4 + out->len + 1;
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

    case RPI_FWREQ_GET_TEMPERATURE:
        if (!s->avs_monitor) {
            return false;
        }
        answer = bcm2711_avs_monitor_get_temperature(s->avs_monitor);
        break;

    case RPI_FWREQ_GET_MAX_TEMPERATURE:
        answer = TEMP_LIMIT;
        break;

    case RPI_FWREQ_GET_RTC_REG:
    case RPI_FWREQ_SET_RTC_REG:
        answer = bcm2712_property_rtc(s, id, tag == RPI_FWREQ_SET_RTC_REG,
                                      arg);
        break;

    case RPI_FWREQ_SET_REBOOT_FLAGS:
        s->reboot_flags = id;
        trace_bcm2712_property_reboot_flags(s->reboot_flags);
        /* fall through */
    case RPI_FWREQ_GET_REBOOT_FLAGS:
        *resplen = bcm2835_property_answer32(ps, value, bufsize,
                                             s->reboot_flags);
        return true;

    case RPI_FWREQ_GET_GENCMD_RESULT:
        *resplen = bcm2712_property_gencmd_tag(s, value, bufsize);
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

static int bcm2712_property_post_load(void *opaque, int version_id)
{
    bcm2712_property_rtc_schedule(opaque);
    return 0;
}

static const VMStateDescription vmstate_bcm2712_property = {
    .name = TYPE_BCM2712_PROPERTY,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = bcm2712_property_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parent_obj, BCM2712PropertyState, 0,
                       vmstate_bcm2835_property, BCM2835PropertyState),
        VMSTATE_UINT32_ARRAY(clock_rate, BCM2712PropertyState,
                             BCM2712_PROPERTY_NUM_CLOCKS),
        VMSTATE_UINT32(clocks_on, BCM2712PropertyState),
        VMSTATE_UINT32(domains_on, BCM2712PropertyState),
        VMSTATE_UINT32(devices_on, BCM2712PropertyState),
        VMSTATE_UINT32(reboot_flags, BCM2712PropertyState),
        VMSTATE_UINT32(rtc_offset, BCM2712PropertyState),
        VMSTATE_UINT32(rtc_alarm, BCM2712PropertyState),
        VMSTATE_BOOL(rtc_alarm_enabled, BCM2712PropertyState),
        VMSTATE_BOOL(rtc_alarm_pending, BCM2712PropertyState),
        VMSTATE_UINT32(rtc_charge_uv, BCM2712PropertyState),
        VMSTATE_END_OF_LIST()
    }
};

/* The real-time clock starts as QEMU's RTC, and a reset leaves it be */
static void bcm2712_property_init(Object *obj)
{
    BCM2712PropertyState *s = BCM2712_PROPERTY(obj);
    struct tm tm;

    qemu_get_timedate(&tm, 0);
    s->rtc_offset = mktimegm(&tm) -
                    qemu_clock_get_ns(rtc_clock) / NANOSECONDS_PER_SECOND;
    s->rtc_timer = timer_new_ns(rtc_clock, bcm2712_property_rtc_alarm, s);
}

static void bcm2712_property_finalize(Object *obj)
{
    BCM2712PropertyState *s = BCM2712_PROPERTY(obj);

    timer_free(s->rtc_timer);
}

static const Property bcm2712_property_properties[] = {
    DEFINE_PROP_LINK("avs-monitor", BCM2712PropertyState, avs_monitor,
                     TYPE_BCM2711_AVS_MONITOR, BCM2711AVSMonitorState *),
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
    device_class_set_props(dc, bcm2712_property_properties);
}

static const TypeInfo bcm2712_property_types[] = {
    {
        .name = TYPE_BCM2712_PROPERTY,
        .parent = TYPE_BCM2835_PROPERTY,
        .instance_size = sizeof(BCM2712PropertyState),
        .instance_init = bcm2712_property_init,
        .instance_finalize = bcm2712_property_finalize,
        .class_init = bcm2712_property_class_init,
    },
};

DEFINE_TYPES(bcm2712_property_types)
