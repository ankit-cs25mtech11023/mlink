#!/bin/bash
# P8.1: build the upstream Zephyr ivshmem doorbell sample with our overrides and
# boot it (TCG) as a third peer on the mlink ivshmem-server.
# usage: zephyr-p81.sh build | run [seconds]
set -euo pipefail
W=$HOME/D/mlink-work
Z=$W/zephyr
cd "$Z"
. .venv/bin/activate
export ZEPHYR_SDK_INSTALL_DIR=$Z/zephyr-sdk-1.0.1 ZEPHYR_BASE=$Z/zephyr
B=$Z/build-p81

case "${1:-}" in
build)
	west build -p always -b qemu_x86_64 -d "$B" \
		"$ZEPHYR_BASE/samples/drivers/virtualization/ivshmem/doorbell" -- \
		-DEXTRA_CONF_FILE="$HOME/D/mlink/zephyr/p81-doorbell.conf"
	ninja -C "$B" zephyr/qemu_locore_image_target zephyr/qemu_main_image_target
	size "$B/zephyr/zephyr.elf" || true
	;;
run)
	: > "$W/logs/zephyr-serial.log"
	timeout "${2:-60}" qemu-system-x86_64 -m 64 \
		-cpu qemu64,+x2apic,mmx,mmxext,sse,sse2 -machine q35 -machine acpi=off \
		-net none -no-reboot -nographic -monitor none \
		-serial file:"$W/logs/zephyr-serial.log" \
		-device ivshmem-doorbell,vectors=5,chardev=ivs \
		-chardev socket,path="$W/run/ivshmem.sock",id=ivs \
		-device loader,file="$B/zephyr/zephyr-qemu-main.elf" -smp cpus=2 \
		-kernel "$B/zephyr/zephyr-qemu-locore.elf" || true
	;;
*)
	echo "usage: $0 build | run [seconds]" >&2
	exit 1
	;;
esac
