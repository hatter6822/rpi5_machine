# SPDX-License-Identifier: GPL-2.0-or-later
#
# A flattened device tree as the tests compare it: every node's path with
# its properties in the order of the blob, and the values printed the
# same way whatever dtc is installed.
#
#   python3 tests/smoke/fdt.py DTB          print the tree
#   python3 tests/smoke/fdt.py OLD NEW      print what NEW changes in OLD

import struct
import sys

FDT_MAGIC = 0xd00dfeed
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9

# Properties whose values change from run to run, printed as RANDOM
RANDOM = {"kaslr-seed", "rng-seed"}


def parse(blob):
    """Return {path: {property: value}} for the tree in @blob, with nodes
    and properties in the blob's order."""
    (magic, _, off_struct, off_strings, _, _, _, _, _,
     size_struct) = struct.unpack_from(">10I", blob)
    if magic != FDT_MAGIC:
        raise ValueError("not a flattened device tree")
    strings = blob[off_strings:]
    tree, stack, pos = {}, [], off_struct
    while pos < off_struct + size_struct:
        (token,) = struct.unpack_from(">I", blob, pos)
        pos += 4
        if token == FDT_BEGIN_NODE:
            end = blob.index(b"\0", pos)
            stack.append(blob[pos:end].decode())
            pos = (end + 4) & ~3
            path = "/" + "/".join(stack[1:])
            tree[path] = {}
        elif token == FDT_END_NODE:
            stack.pop()
        elif token == FDT_PROP:
            size, name_off = struct.unpack_from(">II", blob, pos)
            pos += 8
            name = strings[name_off:strings.index(b"\0", name_off)].decode()
            tree["/" + "/".join(stack[1:])][name] = blob[pos:pos + size]
            pos = (pos + size + 3) & ~3
        elif token == FDT_END:
            break
        elif token != FDT_NOP:
            raise ValueError(f"bad token {token:#x} at {pos - 4:#x}")
    return tree


def load(path):
    with open(path, "rb") as f:
        return parse(f.read())


def value(name, data):
    """@data as dtc would print it: strings, cells or bytes."""
    if name in RANDOM:
        return "RANDOM"
    if not data:
        return ""
    if data[-1] == 0 and all(32 <= b < 127 for b in data[:-1] if b):
        parts = data[:-1].split(b"\0")
        if all(parts) or data == b"\0":
            return ", ".join(f'"{p.decode()}"' for p in parts)
    if len(data) % 4 == 0:
        cells = struct.unpack(f">{len(data) // 4}I", data)
        return "<" + " ".join(f"{c:#x}" for c in cells) + ">"
    return "[" + " ".join(f"{b:02x}" for b in data) + "]"


def dump(tree):
    """The lines that print @tree."""
    lines = []
    for path, props in tree.items():
        lines.append(path)
        for name, data in props.items():
            lines.append(f"    {name} = {value(name, data)}".rstrip(" ="))
    return lines


def diff(old, new):
    """The lines that print what @new adds (+), changes (~) and removes (-)
    in @old, node by node in @new's order, then the nodes it removes."""
    lines = []
    for path, props in new.items():
        before = old.get(path)
        if before is None:
            lines.append(f"+ {path}")
            before = {}
        for name, data in props.items():
            if name not in before:
                lines.append(f"+ {path}:{name} = {value(name, data)}")
            elif before[name] != data:
                lines.append(f"~ {path}:{name} = {value(name, data)} "
                             f"(was {value(name, before[name])})")
        for name in before:
            if name not in props:
                lines.append(f"- {path}:{name}")
    lines += [f"- {path}" for path in old if path not in new]
    return lines


if __name__ == "__main__":
    if len(sys.argv) == 2:
        print("\n".join(dump(load(sys.argv[1]))))
    elif len(sys.argv) == 3:
        print("\n".join(diff(load(sys.argv[1]), load(sys.argv[2]))))
    else:
        sys.exit("usage: fdt.py DTB | fdt.py OLD NEW")
