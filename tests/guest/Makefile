# SPDX-License-Identifier: GPL-2.0-or-later
#
# Bare-metal guests: the test library in lib/, the hello smoke guest and
# the test suite (tests/guest/README.md). Built with clang and lld by default so that no
# aarch64 GCC cross toolchain is required.

# Make's built-in defaults (cc, ld) are host tools; prefer clang and lld
ifeq ($(origin CC),default)
CC      := clang
endif
ifeq ($(origin LD),default)
LD      := ld.lld
endif
OUT     ?= build

TARGET_FLAGS := --target=aarch64-none-elf -mcpu=cortex-a76
CFLAGS  := $(TARGET_FLAGS) -std=gnu11 -O2 -g -Wall -Wextra -Werror \
           -ffreestanding -fno-builtin -nostdlib -fno-pic -fno-stack-protector \
           -mgeneral-regs-only -mstrict-align -Ilib/include \
           -fno-delete-null-pointer-checks   # RAM starts at address 0
ASFLAGS := $(TARGET_FLAGS) -g
LDFLAGS := -nostdlib -static --no-dynamic-linker -T lib/link.ld

# The runtime every guest links; the test runner (test.c) provides
# bm_main() for the suite
LIB_SRCS := start.S console.c exception.c fdt.c gic.c mbox.c pm.c psci.c \
            rng.c runtime.c string.c
LIB_OBJS := $(patsubst %,$(OUT)/lib/%.o,$(basename $(LIB_SRCS)))
SUITE_OBJS := $(OUT)/lib/test.o $(patsubst %.c,$(OUT)/%.o,$(wildcard suite/*.c))

GUESTS  := hello suite

.PHONY: all clean
all: $(GUESTS:%=$(OUT)/%.elf)

$(OUT)/hello.elf: $(LIB_OBJS) $(OUT)/hello/hello.o lib/link.ld
	$(LD) $(LDFLAGS) -o $@ $(filter %.o,$^)

$(OUT)/suite.elf: $(LIB_OBJS) $(SUITE_OBJS) lib/link.ld
	$(LD) $(LDFLAGS) -o $@ $(filter %.o,$^)

$(OUT)/%.o: %.c $(wildcard lib/include/bm/*.h)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -c -o $@ $<

clean:
	rm -rf $(OUT)
