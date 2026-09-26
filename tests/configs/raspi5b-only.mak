# SPDX-License-Identifier: GPL-2.0-or-later
#
# Device configuration for a qemu-system-aarch64 that contains the raspi5b
# machine and nothing else (configure --without-default-devices). It proves
# that the machine selects every device it needs, independently of the
# other Raspberry Pi boards (CONFIG_RASPI). See 'make check-minimal'.

CONFIG_RASPI5=y

# QEMU 11.1 builds target/arm/tcg/gicv5-cpuif.c unconditionally, and TCG
# calls it for every CPU, but its GICv5 helpers are built only with
# ARM_GICV5, so an AArch64 TCG build fails to link without it. Only the
# CPU interface registers of CPUs with FEAT_GCIE come from it; the
# Cortex-A76 has none.
CONFIG_ARM_GICV5=y
