/*
 * pcie: the /init with which the PCIe tests boot Raspberry Pi OS's kernel
 * from an NVMe drive.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * It runs from the root file system on the drive. It loads the modules
 * of the Intel igb Ethernet driver that the drive holds in /lib/modules,
 * brings eth0 up and sends Ethernet frames of a local experimental type
 * to the broadcast address, each "raspi5b pcie ping", until a frame of
 * that type comes back saying "raspi5b pcie pong", which the test on the
 * other end of the link sends for each ping. It reports each step
 * through the kernel log, which reaches the console, and powers off.
 *
 * A Linux program without a C library: it makes its system calls itself.
 */

typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

/* The asm-generic system call numbers, which arm64 has */
#define SYS_ioctl           29
#define SYS_mkdirat         34
#define SYS_mount           40
#define SYS_openat          56
#define SYS_close           57
#define SYS_write           64
#define SYS_sync            81
#define SYS_nanosleep       101
#define SYS_reboot          142
#define SYS_socket          198
#define SYS_bind            200
#define SYS_sendto          206
#define SYS_recvfrom        207
#define SYS_setsockopt      208
#define SYS_finit_module    273

#define AT_FDCWD            (-100)
#define O_RDONLY            0
#define O_WRONLY            1
#define O_CLOEXEC           02000000

#define REBOOT_MAGIC1       0xfee1deadL
#define REBOOT_MAGIC2       0x28121969L
#define REBOOT_POWER_OFF    0x4321fedcL

#define AF_INET             2
#define AF_PACKET           17
#define SOCK_DGRAM          2
#define SOCK_RAW            3
#define SOL_SOCKET          1
#define SO_RCVTIMEO         20

#define SIOCGIFFLAGS        0x8913
#define SIOCSIFFLAGS        0x8914
#define SIOCGIFHWADDR       0x8927
#define SIOCGIFINDEX        0x8933
#define IFF_UP              0x1

/* The frames' type: IEEE 802's first local experimental EtherType */
#define ETH_TYPE            0x88b5
#define ETH_ALEN            6
#define ETH_HLEN            14
#define ETH_ZLEN            60

#define IFNAME              "eth0"
#define PING                "raspi5b pcie ping"
#define PONG                "raspi5b pcie pong"

/* How long eth0 has to appear, and the pong, in 10 ms and 100 ms steps */
#define IF_WAIT             1000
#define PING_TRIES          300

static const char *const modules[] = {
    "/lib/modules/i2c-algo-bit.ko",
    "/lib/modules/igb.ko",
};

struct ifreq {
    char name[16];
    union {
        u16 flags;
        int index;
        struct {
            u16 family;
            u8 data[14];
        } addr;
        u8 pad[24];
    };
};

struct sockaddr_ll {
    u16 family;
    u16 protocol;
    int ifindex;
    u16 hatype;
    u8 pkttype;
    u8 halen;
    u8 addr[8];
};

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

static u16 be16(u16 value)
{
    return (u16)(value << 8 | value >> 8);
}

static int kmsg = -1;

/* A line in the kernel log: "pcie: " @a @b */
static void say(const char *a, const char *b)
{
    static char line[256];
    const char *parts[] = { "<5>pcie: ", a, b };
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

static void sleep_ms(s64 ms)
{
    struct { s64 sec, nsec; } step = { ms / 1000, ms % 1000 * 1000000 };

    syscall(SYS_nanosleep, &step, 0, 0, 0);
}

static void power_off(void)
{
    syscall(SYS_sync, 0, 0, 0, 0);
    syscall(SYS_reboot, REBOOT_MAGIC1, REBOOT_MAGIC2, REBOOT_POWER_OFF, 0);
    for (;;) {
        sleep_ms(10);
    }
}

static void fail(const char *what)
{
    say("FAILED: ", what);
    power_off();
}

static int open_file(const char *path, s64 flags)
{
    return syscall(SYS_openat, AT_FDCWD, path, flags | O_CLOEXEC, 0);
}

static void mount(const char *source, const char *target, const char *type)
{
    syscall(SYS_mkdirat, AT_FDCWD, target, 0755, 0);
    if (syscall6(SYS_mount, (s64)source, (s64)target, (s64)type, 0, 0, 0)) {
        fail(target);
    }
}

static void load_module(const char *path)
{
    int fd = open_file(path, O_RDONLY);

    if (fd < 0 || syscall(SYS_finit_module, fd, "", 0, 0)) {
        fail(path);
    }
    syscall(SYS_close, fd, 0, 0, 0);
    say("loaded ", path);
}

/* The MAC address as text, in @out of 18 bytes */
static void mac_text(char *out, const u8 *mac)
{
    for (int i = 0; i < ETH_ALEN; i++) {
        out[3 * i] = "0123456789abcdef"[mac[i] >> 4];
        out[3 * i + 1] = "0123456789abcdef"[mac[i] & 15];
        out[3 * i + 2] = i < ETH_ALEN - 1 ? ':' : 0;
    }
}

void pcie(void);

void pcie(void)
{
    static u8 frame[ETH_ZLEN], reply[1536];
    struct { s64 sec, usec; } timeout = { 0, 100000 };
    struct sockaddr_ll ll;
    struct ifreq ifr;
    char mac[18];
    int ctl, sock, i;

    /* The kernel mounts devtmpfs on /dev when it is built to */
    kmsg = open_file("/dev/kmsg", O_WRONLY);
    if (kmsg < 0) {
        mount("devtmpfs", "/dev", "devtmpfs");
        kmsg = open_file("/dev/kmsg", O_WRONLY);
    }
    say("running from the root file system", "");
    for (unsigned m = 0; m < sizeof(modules) / sizeof(modules[0]); m++) {
        load_module(modules[m]);
    }

    /* eth0 appears once igb has probed the device */
    ctl = syscall(SYS_socket, AF_INET, SOCK_DGRAM, 0, 0);
    memset(&ifr, 0, sizeof(ifr));
    memcpy(ifr.name, IFNAME, sizeof(IFNAME));
    for (i = 0; i < IF_WAIT; i++) {
        if (!syscall(SYS_ioctl, ctl, SIOCGIFINDEX, &ifr, 0)) {
            break;
        }
        sleep_ms(10);
    }
    if (i == IF_WAIT) {
        fail(IFNAME);
    }
    memset(&ll, 0, sizeof(ll));
    ll.family = AF_PACKET;
    ll.protocol = be16(ETH_TYPE);
    ll.ifindex = ifr.index;
    ll.halen = ETH_ALEN;
    memset(ll.addr, 0xff, ETH_ALEN);

    if (syscall(SYS_ioctl, ctl, SIOCGIFHWADDR, &ifr, 0)) {
        fail("SIOCGIFHWADDR");
    }
    mac_text(mac, ifr.addr.data);
    say(IFNAME " has address ", mac);
    memset(frame, 0xff, ETH_ALEN);
    memcpy(frame + ETH_ALEN, ifr.addr.data, ETH_ALEN);
    frame[12] = ETH_TYPE >> 8;
    frame[13] = ETH_TYPE & 0xff;
    memcpy(frame + ETH_HLEN, PING, length(PING));

    if (syscall(SYS_ioctl, ctl, SIOCGIFFLAGS, &ifr, 0)) {
        fail("SIOCGIFFLAGS");
    }
    ifr.flags |= IFF_UP;
    if (syscall(SYS_ioctl, ctl, SIOCSIFFLAGS, &ifr, 0)) {
        fail("SIOCSIFFLAGS");
    }
    say(IFNAME " is up", "");

    sock = syscall(SYS_socket, AF_PACKET, SOCK_RAW, be16(ETH_TYPE), 0);
    if (sock < 0 || syscall(SYS_bind, sock, &ll, sizeof(ll), 0) ||
        syscall6(SYS_setsockopt, sock, SOL_SOCKET, SO_RCVTIMEO,
                 (s64)&timeout, sizeof(timeout), 0)) {
        fail("packet socket");
    }

    /* Until the link is up, frames go nowhere: send until one comes back */
    for (i = 0; i < PING_TRIES; i++) {
        s64 got;

        syscall6(SYS_sendto, sock, (s64)frame, sizeof(frame), 0, (s64)&ll,
                 sizeof(ll));
        got = syscall6(SYS_recvfrom, sock, (s64)reply, sizeof(reply) - 1, 0,
                       0, 0);
        if (got >= ETH_HLEN + (s64)length(PONG) &&
            starts_with((const char *)reply + ETH_HLEN, PONG)) {
            reply[ETH_HLEN + length(PONG)] = 0;
            say("received ", (const char *)reply + ETH_HLEN);
            say("powering off", "");
            power_off();
        }
    }
    fail("no reply");
}

/* The kernel enters with the stack holding argc, argv and envp, unused */
__asm__(".global _start\n"
        "_start:\n"
        "    bl pcie\n"
        "    b .\n");
