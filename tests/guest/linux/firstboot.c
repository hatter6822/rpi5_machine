/*
 * firstboot: the /init with which the rpi5-boot tests boot Raspberry Pi
 * OS's kernel, from an initramfs.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * On a boot whose command line has Raspberry Pi OS's first-boot init=, it
 * does to the SD card what that first boot does before it reboots: it
 * gives the card a new disk identifier, which renames every partition's
 * PARTUUID, rewrites cmdline.txt in the boot partition to match, without
 * the init=, and reboots. The boot that follows, from the new command
 * line, powers off. Each boot reports its command line and the card's
 * disk identifier through the kernel log, which reaches every console
 * (Raspberry Pi OS's cmdline.txt makes tty1 /dev/console).
 *
 * A Linux program without a C library: it makes its system calls itself.
 */

typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;
typedef unsigned char u8;

/* The asm-generic system call numbers, which arm64 has */
#define SYS_mkdirat         34
#define SYS_umount2         39
#define SYS_mount           40
#define SYS_openat          56
#define SYS_close           57
#define SYS_read            63
#define SYS_write           64
#define SYS_pread64         67
#define SYS_pwrite64        68
#define SYS_sync            81
#define SYS_fsync           82
#define SYS_nanosleep       101
#define SYS_reboot          142

#define AT_FDCWD            (-100)
#define O_RDONLY            0
#define O_WRONLY            1
#define O_RDWR              2
#define O_TRUNC             01000

#define REBOOT_MAGIC1       0xfee1deadL
#define REBOOT_MAGIC2       0x28121969L
#define REBOOT_RESTART      0x01234567L
#define REBOOT_POWER_OFF    0x4321fedcL

/* What cmdline.txt has until Raspberry Pi OS's first boot is over */
#define FIRSTBOOT           " init=/usr/lib/raspberrypi-sys-mods/firstboot"

/* The MBR's disk identifier, and the one this program gives the card */
#define DISK_ID_OFFSET      440
#define NEW_DISK_ID         0x5eed2712u

#define CARD                "/dev/mmcblk0"
#define BOOT_PARTITION      "/dev/mmcblk0p1"

/* How long the card has to appear, in 10 ms steps */
#define CARD_WAIT           2000

static s64 syscall6(s64 n, s64 a, s64 b, s64 c, s64 d, s64 e, s64 f)
{
    register s64 x8 __asm__("x8") = n;
    register s64 x0 __asm__("x0") = a;
    register s64 x1 __asm__("x1") = b;
    register s64 x2 __asm__("x2") = c;
    register s64 x3 __asm__("x3") = d;
    register s64 x4 __asm__("x4") = e;
    register s64 x5 __asm__("x5") = f;

    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory");
    return x0;
}

#define syscall(n, a, b, c, d) \
    syscall6(n, (s64)(a), (s64)(b), (s64)(c), (s64)(d), 0, 0)

/* The compiler may call these for copies and clears */
void *memcpy(void *dst, const void *src, u64 n)
{
    u8 *d = dst;
    const u8 *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int c, u64 n)
{
    u8 *d = dst;

    while (n--) {
        *d++ = (u8)c;
    }
    return dst;
}

static u64 length(const char *s)
{
    u64 n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static int starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

/* Where @needle first is in @s, or 0 */
static const char *find(const char *s, const char *needle)
{
    for (; *s; s++) {
        if (starts_with(s, needle)) {
            return s;
        }
    }
    return 0;
}

static void hex32(char *out, u32 value)
{
    for (int i = 7; i >= 0; i--) {
        out[i] = "0123456789abcdef"[value & 15];
        value >>= 4;
    }
    out[8] = 0;
}

static int kmsg = -1;

/* A line in the kernel log: "firstboot: " @a @b */
static void say(const char *a, const char *b)
{
    static char line[1024];
    const char *parts[] = { "<5>firstboot: ", a, b };
    u64 n = 0;

    for (unsigned i = 0; i < 3; i++) {
        for (const char *p = parts[i]; *p && n < sizeof(line) - 1; p++) {
            line[n++] = *p;
        }
    }
    if (kmsg >= 0) {
        syscall(SYS_write, kmsg, line, n, 0);
    }
}

static void sleep_10ms(void)
{
    struct { s64 sec, nsec; } step = { 0, 10000000 };

    syscall(SYS_nanosleep, &step, 0, 0, 0);
}

static void power(s64 command)
{
    syscall(SYS_sync, 0, 0, 0, 0);
    syscall(SYS_reboot, REBOOT_MAGIC1, REBOOT_MAGIC2, command, 0);
    for (;;) {
        sleep_10ms();
    }
}

static void fail(const char *what)
{
    say("FAILED: ", what);
    power(REBOOT_POWER_OFF);
}

static void mount(const char *source, const char *target, const char *type)
{
    syscall(SYS_mkdirat, AT_FDCWD, target, 0755, 0);
    if (syscall6(SYS_mount, (s64)source, (s64)target, (s64)type, 0, 0, 0)) {
        fail(target);
    }
}

static int open_file(const char *path, s64 flags)
{
    return syscall(SYS_openat, AT_FDCWD, path, flags, 0);
}

/* @path's contents, NUL-terminated, in @buf of @size bytes */
static void read_file(const char *path, char *buf, u64 size)
{
    int fd = open_file(path, O_RDONLY);
    u64 n = 0;
    s64 got;

    if (fd < 0) {
        fail(path);
    }
    while (n < size - 1 &&
           (got = syscall(SYS_read, fd, buf + n, size - 1 - n, 0)) > 0) {
        n += got;
    }
    buf[n] = 0;
    syscall(SYS_close, fd, 0, 0, 0);
}

static void write_file(const char *path, const char *text)
{
    int fd = open_file(path, O_WRONLY | O_TRUNC);
    u64 n = length(text);

    if (fd < 0 || syscall(SYS_write, fd, text, n, 0) != (s64)n ||
        syscall(SYS_fsync, fd, 0, 0, 0)) {
        fail(path);
    }
    syscall(SYS_close, fd, 0, 0, 0);
}

static void wait_for(const char *path)
{
    for (int i = 0; i < CARD_WAIT; i++) {
        int fd = open_file(path, O_RDONLY);

        if (fd >= 0) {
            syscall(SYS_close, fd, 0, 0, 0);
            return;
        }
        sleep_10ms();
    }
    fail(path);
}

/*
 * @text with Raspberry Pi OS's first-boot init= removed and the disk
 * identifier @from, as PARTUUIDs spell it, turned into @to
 */
static void rewrite(char *out, u64 size, const char *text, const char *from,
                    const char *to)
{
    u64 n = 0;

    while (*text && n < size - 9) {
        if (starts_with(text, FIRSTBOOT)) {
            text += length(FIRSTBOOT);
        } else if (starts_with(text, from)) {
            memcpy(out + n, to, 8);
            n += 8;
            text += 8;
        } else {
            out[n++] = *text++;
        }
    }
    out[n] = 0;
}

static char cmdline[4096], text[4096], rewritten[4096];

void firstboot(void);

void firstboot(void)
{
    char id[9], new_id[9];
    u8 raw[4];
    u32 disk_id;
    int card;
    u64 n;

    mount("devtmpfs", "/dev", "devtmpfs");
    kmsg = open_file("/dev/kmsg", O_WRONLY);
    mount("proc", "/proc", "proc");

    read_file("/proc/cmdline", cmdline, sizeof(cmdline));
    n = length(cmdline);
    if (n && cmdline[n - 1] == '\n') {
        cmdline[n - 1] = 0;
    }
    say("command line: ", cmdline);

    wait_for(BOOT_PARTITION);
    card = open_file(CARD, O_RDWR);
    if (card < 0 ||
        syscall(SYS_pread64, card, raw, 4, DISK_ID_OFFSET) != 4) {
        fail(CARD);
    }
    disk_id = raw[0] | raw[1] << 8 | raw[2] << 16 | (u32)raw[3] << 24;
    hex32(id, disk_id);
    say("disk identifier ", id);

    if (!find(cmdline, FIRSTBOOT)) {
        say("powering off", "");
        power(REBOOT_POWER_OFF);
    }

    mount(BOOT_PARTITION, "/boot", "vfat");
    read_file("/boot/cmdline.txt", text, sizeof(text));
    hex32(new_id, NEW_DISK_ID);
    rewrite(rewritten, sizeof(rewritten), text, id, new_id);
    write_file("/boot/cmdline.txt", rewritten);
    if (syscall(SYS_umount2, "/boot", 0, 0, 0)) {
        fail("/boot");
    }

    raw[0] = NEW_DISK_ID & 0xff;
    raw[1] = NEW_DISK_ID >> 8 & 0xff;
    raw[2] = NEW_DISK_ID >> 16 & 0xff;
    raw[3] = NEW_DISK_ID >> 24;
    if (syscall(SYS_pwrite64, card, raw, 4, DISK_ID_OFFSET) != 4 ||
        syscall(SYS_fsync, card, 0, 0, 0)) {
        fail(CARD);
    }
    say("new disk identifier ", new_id);
    say("rebooting", "");
    power(REBOOT_RESTART);
}

/* The kernel enters with the stack holding argc, argv and envp, unused */
__asm__(".global _start\n"
        "_start:\n"
        "    bl firstboot\n"
        "    b .\n");
