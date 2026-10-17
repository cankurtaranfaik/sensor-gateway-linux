#!/bin/sh
# usage: scripts/configure.sh <buildroot-dir> <output-dir>
#
# Start from Buildroot's stock qemu_aarch64_virt_defconfig and apply our
# fragment on top. Later assignments win in Kconfig, so appending is enough.
set -eu

BR=$(realpath "$1")
OUT=$(realpath -m "$2")
EXT=$(realpath "$(dirname "$0")/..")

make -C "$BR" O="$OUT" BR2_EXTERNAL="$EXT" qemu_aarch64_virt_defconfig
cat "$EXT/configs/sensgw.fragment" >> "$OUT/.config"
make -C "$BR" O="$OUT" olddefconfig

# fail early if a symbol from the fragment did not stick
for sym in BR2_PACKAGE_VIMU BR2_PACKAGE_TELEMD BR2_TOOLCHAIN_EXTERNAL_BOOTLIN; do
	grep -q "^$sym=y" "$OUT/.config" || { echo "$sym not set" >&2; exit 1; }
done
