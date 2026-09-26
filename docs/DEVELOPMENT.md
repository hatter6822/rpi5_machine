# Development guide

## The QEMU work tree

`qemu/` is upstream QEMU pinned to a release tag. Our work reaches it in two
ways, both managed by `scripts/qemu-tree` (wrapped by `make`):

| Kind of change | Lives in | Reaches `qemu/` as |
| --- | --- | --- |
| New file (device model, header, test, doc page) | `overlay/<path in QEMU tree>` | a relative symlink |
| Edit to an existing QEMU file (Kconfig, meson.build, trace-events, index pages) | `patches/NNNN-*.patch` | `git apply` to the work tree |

The rule that keeps this manageable: **anything that can be a new file is a
new file.** Patches are reserved for the glue that cannot be.

Patches form a series, applied in file name order, and like the commits of
an upstream series they may change the same file: `hw/arm/Kconfig` is
changed both by the upstream-first Kconfig split (0001) and by the BCM2712
glue (0002). Each patch is generated against the tree with every earlier
patch applied and with the changes of later patches taken back out, so it
can still be refreshed on its own. Two consequences:

* when a patch changes (a checkout, a pull), `apply` reverts it together
  with every later patch that shares a file with it, then reapplies them
  all in order;
* an edit to a shared file shows as a local edit of every patch that
  changes the file (`make status`); refresh the one it belongs to. Edits
  to lines that a later patch changes belong to that later patch.

### Everyday loop

```
make setup            # once: fetch qemu/, apply patches, link overlay
$EDITOR overlay/hw/arm/bcm2712.c
make build check
```

Editing `qemu/hw/arm/bcm2712.c` is equivalent: it is a symlink to the overlay.

### Adding a file

Create it under `overlay/` at its QEMU path, then `make apply` to link it.
New sources also need build glue (`meson.build`, `Kconfig`), which is a patch.

### Changing an existing QEMU file

Edit it in `qemu/`, then either

```
scripts/qemu-tree new hw-misc-add-bcm2712-foo qemu-relative/path ...   # new patch
scripts/qemu-tree refresh 0002-hw-arm-Build-the-BCM2712-SoC-and-raspi5b-machine.patch
```

`new` appends a patch to the series and opens git's editor for a commit
message in QEMU style (`subsystem: Imperative summary`, then the why).
`refresh` regenerates an existing patch from the files it touches, keeping
its message, author and date (parsed with `git mailinfo`), so an unchanged
tree reproduces the patch byte for byte. The script needs bash 4 and GNU coreutils (`realpath
--relative-to`); on macOS install `coreutils` from Homebrew.

`make status` lists edits in `qemu/` that belong to no patch or overlay file;
they are not tracked by this repository and will be lost by `make unapply`.

### Moving to a new QEMU release

```
make unapply
git -C qemu fetch --depth 1 origin tag vX.Y.Z && git -C qemu checkout vX.Y.Z
make apply            # fix any patch that no longer applies, then refresh it
make build check checkpatch
git add qemu patches overlay && git commit
```

Upstream API churn (header moves such as `hw/boards.h` becoming
`hw/core/boards.h` in 11.x) shows up as build failures in overlay sources.

## Coding conventions

We follow upstream QEMU conventions so the code can be submitted unchanged:

* `docs/devel/style.rst` and `scripts/checkpatch.pl` (`make checkpatch` must be clean).
* One device per file under the matching `hw/<subsystem>/`, header in
  `include/hw/<subsystem>/`, `SPDX-License-Identifier: GPL-2.0-or-later`.
* QOM: `OBJECT_DECLARE_SIMPLE_TYPE`, `DEFINE_TYPES`, children embedded in the
  parent state and created with `object_initialize_child()`, configuration
  through `qdev` properties, never globals.
* Registers: `REG32()`/`FIELD()` from `hw/core/registerfields.h`;
  `MemoryRegionOps` with explicit `.valid` and `.impl` access sizes and
  `.endianness = DEVICE_LITTLE_ENDIAN`.
* Guest mistakes are `qemu_log_mask(LOG_GUEST_ERROR, ...)`, missing features
  `LOG_UNIMP`; never `abort()` on guest-controlled input.
* Every stateful device has a `VMStateDescription` and implements reset
  through the `Resettable` interface (or `device_class_set_legacy_reset` when
  matching an existing model).
* Observable behaviour gets `trace-events` entries (a patch to that
  directory's `trace-events` file).
* Values that come from hardware documentation or device trees cite their
  source in a comment; values that are guesses say so and carry a
  `TODO(WSx.y)` referencing [PLAN.md](PLAN.md).

## Tests

| Layer | Location | Run by |
| --- | --- | --- |
| Register-level device tests | `overlay/tests/qtest/*-test.c` | `make check-qtest` |
| Bare-metal guests over UART | `tests/guest/`, `tests/smoke/` | `make check-smoke` |
| Linux / firmware boots | `overlay/tests/functional/aarch64/` (planned, WS9.3) | QEMU functional test runner |

Each new device lands with a qtest for its registers and reset values. The
bare-metal guests are built with `clang --target=aarch64-none-elf` and `lld`,
so no cross GCC is needed; keep them freestanding and MMU-off safe (aligned
accesses only, `-mstrict-align`).

### Trying Linux

The Raspberry Pi firmware repository carries a current kernel and DTB:

```
base=https://raw.githubusercontent.com/raspberrypi/firmware/master/boot
curl -LO $base/kernel_2712.img -LO $base/bcm2712-rpi-5-b.dtb
build/qemu-system-aarch64 -M raspi5b -m 4G -nographic \
    -kernel kernel_2712.img -dtb bcm2712-rpi-5-b.dtb \
    -append "console=ttyAMA10,115200 earlycon=pl011,mmio32,0x107d001000"
```

Without storage the boot currently ends at the root-fs mount (expected).

### Debugging

* `-d unimp,guest_errors` shows accesses to unmodelled blocks by name
  (`bcm2712.mbox`, `bcm2712.soc`, ...).
* `-s -S` and `gdb-multiarch` for guest debugging; `-trace 'bcm2712*'` once
  trace points exist.
* `-machine dumpdtb=out.dtb` shows the device tree after QEMU's fix-ups.

## Upstreaming

The end state is a patch series on qemu-devel. The overlay/patch split maps
onto it directly: each device model is one commit (overlay files plus its
glue), followed by tests and documentation. Before submitting:

* every commit needs a `Signed-off-by:` from its human author (the DCO,
  `docs/devel/submitting-a-patch.rst`); the patches here deliberately carry
  none, so `make checkpatch` passes `--no-signoff`;
* add a `MAINTAINERS` entry for the new files;
* run the full `make check` of QEMU, not only ours.
