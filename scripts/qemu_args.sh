# Sourced by the other scripts. The DTB dump and the real boot must use the
# same machine options, otherwise the dumped tree does not match the machine.
# shellcheck shell=sh
QEMU_MACHINE="virt"
QEMU_CPU_ARGS="-cpu cortex-a53 -smp 1 -m 256M"
QEMU_DEVICE_ARGS="-netdev user,id=eth0 -device virtio-net-device,netdev=eth0"
