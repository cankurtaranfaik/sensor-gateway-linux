#!/bin/sh
# usage: scripts/make_dtb.sh <images-dir>
#
# QEMU builds the device tree for "virt" itself. Dump it, include it from a
# small wrapper that adds the vimu node and compile the result to vimu.dtb.
set -eu

IMAGES=$(realpath "$1")
HERE=$(dirname "$(realpath "$0")")
. "$HERE/qemu_args.sh"

# shellcheck disable=SC2086
qemu-system-aarch64 -M "$QEMU_MACHINE,dumpdtb=$IMAGES/virt.dtb" \
	$QEMU_CPU_ARGS $QEMU_DEVICE_ARGS -display none
dtc -q -I dtb -O dts -o "$IMAGES/virt.dts" "$IMAGES/virt.dtb"

# virt.dts carries the /dts-v1/ header, the second include merges into "/"
cat > "$IMAGES/vimu.dts" <<EOF
/include/ "virt.dts"
/include/ "$HERE/../board/sensgw/vimu-node.dtsi"
EOF

dtc -q -I dts -O dtb -o "$IMAGES/vimu.dtb" "$IMAGES/vimu.dts"
echo "wrote $IMAGES/vimu.dtb"
