#!/bin/sh
# usage: scripts/run_and_measure.sh <images-dir> <results-dir> [capture_sec]
#
# Boot the image in QEMU, listen for telemetry on the host side for
# capture_sec seconds after the first frame, then stop QEMU and write a report.
set -eu

IMAGES=$(realpath "$1")
RESULTS=$(realpath -m "$2")
CAPTURE=${3:-30}
HERE=$(dirname "$(realpath "$0")")
. "$HERE/qemu_args.sh"

mkdir -p "$RESULTS"

ROOTFS="$IMAGES/rootfs.ext4"
[ -e "$ROOTFS" ] || ROOTFS="$IMAGES/rootfs.ext2"

python3 "$HERE/../host/telem_rx.py" --port 5005 --capture "$CAPTURE" \
	--timeout 600 --out "$RESULTS" &
RX_PID=$!
sleep 1

date +%s.%N > "$RESULTS/qemu_start"
# shellcheck disable=SC2086
qemu-system-aarch64 -M "$QEMU_MACHINE" $QEMU_CPU_ARGS $QEMU_DEVICE_ARGS \
	-kernel "$IMAGES/Image" -dtb "$IMAGES/vimu.dtb" \
	-append "rootwait root=/dev/vda console=ttyAMA0" \
	-drive file="$ROOTFS",if=none,format=raw,id=hd0 \
	-device virtio-blk-device,drive=hd0 \
	-display none -monitor none -serial file:"$RESULTS/serial.log" &
QEMU_PID=$!

RC=0
wait "$RX_PID" || RC=$?
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

python3 "$HERE/../host/report.py" --images "$IMAGES" --results "$RESULTS"
exit "$RC"
