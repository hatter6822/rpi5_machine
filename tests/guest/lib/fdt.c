/*
 * A read-only flattened device tree walker.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Follows the Devicetree Specification v0.4, chapter 5 (flattened format)
 * and section 2.3.8 ("ranges"). The blob is only read, a byte or an
 * aligned 32-bit word at a time, which is safe on Device memory.
 */

#include <bm/fdt.h>
#include <bm/io.h>

#define FDT_MAGIC               0xd00dfeed
#define FDT_BEGIN_NODE          1
#define FDT_END_NODE            2
#define FDT_PROP                3
#define FDT_NOP                 4
#define FDT_END                 9

#define FDT_MAX_DEPTH           16
#define FDT_MAX_SIZE            (2u << 20)

/* Header fields, in 32-bit words */
enum {
    HDR_MAGIC, HDR_TOTALSIZE, HDR_OFF_STRUCT, HDR_OFF_STRINGS,
    HDR_OFF_MEM_RSVMAP, HDR_VERSION, HDR_LAST_COMP_VERSION,
    HDR_BOOT_CPUID, HDR_SIZE_STRINGS, HDR_SIZE_STRUCT, HDR_WORDS
};

/* The tree may sit at physical address 0, so validity is a separate flag */
static const uint8_t *fdt;
static bool valid;
static uint32_t off_struct, size_struct, off_strings, size_strings;

static uint32_t be32(const void *p)
{
    const volatile uint32_t *w = p;
    uint32_t v = *w;

    return __builtin_bswap32(v);
}

uint32_t fdt_cell(const void *prop, unsigned index)
{
    return be32((const uint32_t *)prop + index);
}

bool fdt_prop_u32(int node, const char *name, uint32_t *val)
{
    uint32_t len;
    const void *prop = fdt_getprop(node, name, &len);

    if (!prop || len != 4) {
        return false;
    }
    *val = fdt_cell(prop, 0);
    return true;
}

bool fdt_prop_u64(int node, const char *name, uint64_t *val)
{
    uint32_t len;
    const void *prop = fdt_getprop(node, name, &len);

    if (!prop || len != 8) {
        return false;
    }
    *val = (uint64_t)fdt_cell(prop, 0) << 32 | fdt_cell(prop, 1);
    return true;
}

static uint32_t hdr(const void *blob, unsigned field)
{
    return fdt_cell(blob, field);
}

bool fdt_init(const void *blob)
{
    uint32_t total;

    valid = false;
    /* Physical address 0 is RAM, where QEMU puts the tree for an ELF */
    if (((uintptr_t)blob & 7) || hdr(blob, HDR_MAGIC) != FDT_MAGIC ||
        hdr(blob, HDR_LAST_COMP_VERSION) > 17 || hdr(blob, HDR_VERSION) < 17) {
        return false;
    }
    total = hdr(blob, HDR_TOTALSIZE);
    off_struct = hdr(blob, HDR_OFF_STRUCT);
    size_struct = hdr(blob, HDR_SIZE_STRUCT);
    off_strings = hdr(blob, HDR_OFF_STRINGS);
    size_strings = hdr(blob, HDR_SIZE_STRINGS);
    if (total > FDT_MAX_SIZE || (off_struct & 3) ||
        off_struct + size_struct > total ||
        off_strings + size_strings > total) {
        return false;
    }
    fdt = blob;
    valid = true;
    return true;
}

bool fdt_present(void)
{
    return valid;
}

uintptr_t fdt_address(void)
{
    return (uintptr_t)fdt;
}

uint32_t fdt_size(void)
{
    return valid ? hdr(fdt, HDR_TOTALSIZE) : 0;
}

static uint32_t token(uint32_t off)
{
    return off + 4 <= size_struct ? be32(fdt + off_struct + off) : FDT_END;
}

static const char *sbase(uint32_t off)
{
    return (const char *)fdt + off_struct + off;
}

static size_t str_len(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static bool str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static uint32_t align4(uint32_t v)
{
    return (v + 3) & ~3u;
}

/*
 * Offset of the token after the one at @off. FDT_END and malformed tokens
 * lead to the end of the structure block, so every walk terminates.
 */
static uint32_t next_token(uint32_t off)
{
    switch (token(off)) {
    case FDT_BEGIN_NODE:
        return off + 4 + align4(str_len(sbase(off + 4)) + 1);
    case FDT_PROP:
        return off + 12 + align4(be32(sbase(off + 4)));
    case FDT_END_NODE:
    case FDT_NOP:
        return off + 4;
    default:
        return size_struct;
    }
}

const char *fdt_node_name(int node)
{
    return sbase(node + 4);
}

/* Offset just after the node's properties, where its children begin */
static uint32_t first_child_token(int node)
{
    uint32_t off = next_token(node);

    while (token(off) == FDT_PROP || token(off) == FDT_NOP) {
        off = next_token(off);
    }
    return off;
}

/* Offset of the token after the node's FDT_END_NODE */
static uint32_t skip_node(int node)
{
    uint32_t off = node;
    int depth = 0;

    for (;;) {
        uint32_t t = token(off);

        if (t == FDT_BEGIN_NODE) {
            depth++;
        } else if (t == FDT_END_NODE && --depth == 0) {
            return off + 4;
        } else if (t == FDT_END) {
            return off;
        }
        off = next_token(off);
    }
}

const void *fdt_getprop(int node, const char *name, uint32_t *len)
{
    if (!valid || node < 0 || token(node) != FDT_BEGIN_NODE) {
        return NULL;
    }
    for (uint32_t off = next_token(node);; off = next_token(off)) {
        uint32_t t = token(off);

        if (t == FDT_NOP) {
            continue;
        }
        if (t != FDT_PROP) {
            return NULL;
        }
        uint32_t plen = be32(sbase(off + 4));
        uint32_t nameoff = be32(sbase(off + 8));

        if (nameoff < size_strings &&
            str_eq((const char *)fdt + off_strings + nameoff, name)) {
            if (len) {
                *len = plen;
            }
            return sbase(off + 12);
        }
    }
}

static bool has_char(const char *s, size_t len, char c)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == c) {
            return true;
        }
    }
    return false;
}

/*
 * Child of @node named @name (@namelen bytes); a name without a unit
 * address also matches a child that has one, as in dtc
 */
static int subnode(int node, const char *name, size_t namelen)
{
    bool unit = has_char(name, namelen, '@');
    uint32_t off = first_child_token(node);

    for (;;) {
        while (token(off) == FDT_NOP) {
            off = next_token(off);
        }
        if (token(off) != FDT_BEGIN_NODE) {
            return -1;
        }

        const char *n = fdt_node_name(off);
        size_t i = 0;

        while (i < namelen && n[i] == name[i]) {
            i++;
        }
        if (i == namelen && (n[i] == '\0' || (n[i] == '@' && !unit))) {
            return off;
        }
        off = skip_node(off);
    }
}

/* The node starting at @off, skipping NOPs, or -1 */
static int node_at(uint32_t off)
{
    while (token(off) == FDT_NOP) {
        off = next_token(off);
    }
    return token(off) == FDT_BEGIN_NODE ? (int)off : -1;
}

int fdt_first_subnode(int node)
{
    if (!valid || node < 0 || token(node) != FDT_BEGIN_NODE) {
        return -1;
    }
    return node_at(first_child_token(node));
}

int fdt_next_subnode(int child)
{
    if (!valid || child < 0 || token(child) != FDT_BEGIN_NODE) {
        return -1;
    }
    return node_at(skip_node(child));
}

int fdt_path_offset(const char *path)
{
    int node = 0;

    if (!valid || path[0] != '/' || token(0) != FDT_BEGIN_NODE) {
        return -1;
    }
    while (*path) {
        size_t len = 0;

        while (*path == '/') {
            path++;
        }
        while (path[len] && path[len] != '/') {
            len++;
        }
        if (!len) {
            break;
        }
        node = subnode(node, path, len);
        if (node < 0) {
            return -1;
        }
        path += len;
    }
    return node;
}

int fdt_alias_offset(const char *alias)
{
    const char *path = fdt_getprop(fdt_path_offset("/aliases"), alias, NULL);

    return path ? fdt_path_offset(path) : -1;
}

/*
 * Walk every node in document order. @fn returns true to stop; the result
 * is the node it stopped at, or -1.
 */
static int for_each_node(bool (*fn)(int node, const void *arg),
                         const void *arg)
{
    if (!valid) {
        return -1;
    }
    for (uint32_t off = 0; token(off) != FDT_END; off = next_token(off)) {
        if (token(off) == FDT_BEGIN_NODE && fn(off, arg)) {
            return off;
        }
    }
    return -1;
}

static bool stringlist_contains(const char *list, uint32_t len,
                                const char *s)
{
    for (uint32_t i = 0; i < len; i += str_len(list + i) + 1) {
        if (str_eq(list + i, s)) {
            return true;
        }
    }
    return false;
}

static bool is_compatible(int node, const void *compatible)
{
    uint32_t len;
    const char *list = fdt_getprop(node, "compatible", &len);

    return list && stringlist_contains(list, len, compatible);
}

bool fdt_node_is_compatible(int node, const char *compatible)
{
    return node >= 0 && is_compatible(node, compatible);
}

int fdt_find_compatible(const char *compatible)
{
    return for_each_node(is_compatible, compatible);
}

static bool has_phandle(int node, const void *phandle)
{
    uint32_t len;
    const void *p = fdt_getprop(node, "phandle", &len);

    return p && len == 4 && fdt_cell(p, 0) == *(const uint32_t *)phandle;
}

bool fdt_node_is_enabled(int node)
{
    const char *status = fdt_getprop(node, "status", NULL);

    return !status || str_eq(status, "okay") || str_eq(status, "ok");
}

/* Ancestors of @node, root first, ending with @node; returns the count */
static int node_path(int node, int path[FDT_MAX_DEPTH])
{
    int depth = 0;

    for (uint32_t off = 0; token(off) != FDT_END; off = next_token(off)) {
        if (token(off) == FDT_BEGIN_NODE) {
            if (depth == FDT_MAX_DEPTH) {
                return -1;
            }
            path[depth++] = off;
            if ((int)off == node) {
                return depth;
            }
        } else if (token(off) == FDT_END_NODE && depth > 0) {
            depth--;
        }
    }
    return -1;
}

static uint32_t cells(int node, const char *name, uint32_t dflt)
{
    uint32_t len;
    const void *p = fdt_getprop(node, name, &len);

    return p && len == 4 ? fdt_cell(p, 0) : dflt;
}

/* Up to two cells as one number; false if the value needs more */
static bool read_cells(const void *prop, unsigned index, uint32_t n,
                       uint64_t *val)
{
    if (n > 2) {
        return false;
    }
    *val = 0;
    for (uint32_t i = 0; i < n; i++) {
        *val = (*val << 32) | fdt_cell(prop, index + i);
    }
    return true;
}

/* Translate @addr from the address space of @bus to that of its parent */
static bool translate(int bus, int parent, uint64_t *addr)
{
    uint32_t len;
    const void *ranges = fdt_getprop(bus, "ranges", &len);
    uint32_t cac = cells(bus, "#address-cells", 2);
    uint32_t csc = cells(bus, "#size-cells", 1);
    uint32_t pac = cells(parent, "#address-cells", 2);
    uint32_t entry = cac + pac + csc;

    if (!ranges) {
        return false;           /* not a memory-mapped bus */
    }
    if (len == 0) {
        return true;            /* identity mapping */
    }
    for (uint32_t i = 0; (i + entry) * 4 <= len; i += entry) {
        uint64_t child, host, size;

        if (!read_cells(ranges, i, cac, &child) ||
            !read_cells(ranges, i + cac, pac, &host) ||
            !read_cells(ranges, i + cac + pac, csc, &size)) {
            return false;
        }
        if (*addr >= child && *addr - child < size) {
            *addr = host + (*addr - child);
            return true;
        }
    }
    return false;
}

bool fdt_reg(int node, unsigned index, uint64_t *addr, uint64_t *size)
{
    int path[FDT_MAX_DEPTH];
    int depth = node_path(node, path);
    uint32_t len, ac, sc;
    const void *reg;

    if (depth < 2) {
        return false;
    }
    ac = cells(path[depth - 2], "#address-cells", 2);
    sc = cells(path[depth - 2], "#size-cells", 1);
    reg = fdt_getprop(node, "reg", &len);
    if (!reg || (index + 1) * (ac + sc) * 4 > len ||
        !read_cells(reg, index * (ac + sc), ac, addr) ||
        !read_cells(reg, index * (ac + sc) + ac, sc, size)) {
        return false;
    }
    for (int i = depth - 2; i > 0; i--) {
        if (!translate(path[i], path[i - 1], addr)) {
            return false;
        }
    }
    return true;
}

int fdt_interrupt_parent(int node)
{
    int path[FDT_MAX_DEPTH];
    int depth = node_path(node, path);

    for (int i = depth - 1; i >= 0; i--) {
        uint32_t len;
        const void *p = fdt_getprop(path[i], "interrupt-parent", &len);

        if (p && len == 4) {
            uint32_t phandle = fdt_cell(p, 0);

            return for_each_node(has_phandle, &phandle);
        }
    }
    return -1;
}

bool fdt_gic_intid(int node, unsigned index, unsigned *intid)
{
    int gic = fdt_interrupt_parent(node);
    uint32_t len;
    const void *irqs = fdt_getprop(node, "interrupts", &len);

    /* GIC binding: <type number flags>, type 0 = SPI, 1 = PPI */
    if (gic < 0 || !fdt_getprop(gic, "interrupt-controller", NULL) ||
        cells(gic, "#interrupt-cells", 0) != 3 ||
        !irqs || (index + 1) * 12 > len) {
        return false;
    }
    switch (fdt_cell(irqs, index * 3)) {
    case 0:
        *intid = 32 + fdt_cell(irqs, index * 3 + 1);
        return true;
    case 1:
        *intid = 16 + fdt_cell(irqs, index * 3 + 1);
        return true;
    default:
        return false;
    }
}

int fdt_stdout_offset(void)
{
    const char *sp = fdt_getprop(fdt_path_offset("/chosen"), "stdout-path",
                                 NULL);
    char buf[128];
    size_t len = 0;

    if (!sp) {
        return -1;
    }
    /* "serial10:115200n8" or "/soc/serial@7d001000:115200n8" */
    while (sp[len] && sp[len] != ':' && len < sizeof(buf) - 1) {
        buf[len] = sp[len];
        len++;
    }
    buf[len] = '\0';
    return buf[0] == '/' ? fdt_path_offset(buf) : fdt_alias_offset(buf);
}
