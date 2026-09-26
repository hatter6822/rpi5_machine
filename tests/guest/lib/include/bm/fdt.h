/*
 * A read-only flattened device tree walker.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Just enough to find devices: nodes by path, alias or compatible string,
 * properties, "reg" entries translated to CPU physical addresses through
 * every parent's "ranges", and GIC interrupt specifiers. Node handles are
 * offsets of FDT_BEGIN_NODE tokens in the structure block; negative
 * values mean "not found".
 */

#ifndef BM_FDT_H
#define BM_FDT_H

#include <stdbool.h>
#include <stdint.h>

/* Use the tree at @blob if it is a valid FDT; returns whether it is */
bool fdt_init(const void *blob);
bool fdt_present(void);
uintptr_t fdt_address(void);
uint32_t fdt_size(void);

int fdt_path_offset(const char *path);
int fdt_alias_offset(const char *alias);
int fdt_find_compatible(const char *compatible);
bool fdt_node_is_compatible(int node, const char *compatible);
const char *fdt_node_name(int node);

/* The first child of @node, or the sibling after @child; -1 if none */
int fdt_first_subnode(int node);
int fdt_next_subnode(int child);

/* Property value and length in bytes, or NULL if absent */
const void *fdt_getprop(int node, const char *name, uint32_t *len);
bool fdt_node_is_enabled(int node);

/* Big-endian cell @index of a property value */
uint32_t fdt_cell(const void *prop, unsigned index);

/*
 * CPU physical address and size of the @index-th "reg" entry, translated
 * through the "ranges" of every ancestor
 */
bool fdt_reg(int node, unsigned index, uint64_t *addr, uint64_t *size);

/*
 * GIC INTID of the @index-th "interrupts" specifier, for nodes whose
 * interrupt parent is a GIC with three-cell specifiers
 */
bool fdt_gic_intid(int node, unsigned index, unsigned *intid);

/* The node that /chosen/stdout-path names, through /aliases if needed */
int fdt_stdout_offset(void);

#endif /* BM_FDT_H */
