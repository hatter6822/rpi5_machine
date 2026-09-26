# SPDX-License-Identifier: GPL-2.0-or-later
#
# Bare-metal guests used by the smoke tests. Built with clang and lld by
# default so that no aarch64 GCC cross toolchain is required.

# Make's built-in defaults (cc, ld) are host tools; prefer clang and lld
ifeq ($(origin CC),default)
CC      := clang
endif
ifeq ($(origin LD),default)
LD      := ld.lld
endif
OUT     ?= build

TARGET_FLAGS := --target=aarch64-none-elf -mcpu=cortex-a76
CFLAGS  := $(TARGET_FLAGS) -std=c11 -O2 -g -Wall -Wextra -Werror \
           -ffreestanding -fno-builtin -nostdlib -fno-pic -fno-stack-protector \
           -mgeneral-regs-only -mstrict-align
ASFLAGS := $(TARGET_FLAGS) -g
LDFLAGS := -nostdlib -static --no-dynamic-linker

GUESTS  := hello

.PHONY: all clean
all: $(GUESTS:%=$(OUT)/%.elf)

$(OUT)/hello.elf: $(OUT)/hello/start.o $(OUT)/hello/hello.o hello/link.ld
	$(LD) $(LDFLAGS) -T hello/link.ld -o $@ $(filter %.o,$^)

$(OUT)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -c -o $@ $<

clean:
	rm -rf $(OUT)
