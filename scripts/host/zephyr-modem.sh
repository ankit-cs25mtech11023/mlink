#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# P8.2/P8.3: build / run / stop the Zephyr modem (zephyr/mlink-modem) as a
# peer on the mlink ivshmem-server. MODE is "loop" (loopback, P8.2, default)
# or "net" (Zephyr IP stack, P8.3). Run "run" inside tmux; it stays in the
# foreground until killed (or for [seconds]).
# usage: zephyr-modem.sh build [loop|net] | run [loop|net] [seconds] | stop | status
set -euo pipefail
W=$HOME/D/mlink-work
Z=$W/zephyr
APP=$HOME/D/mlink/zephyr/mlink-modem
LOG=$W/logs/zephyr-modem-serial.log
NAME=mlink-zephyr-modem

mode_dir() {
	case "${1:-loop}" in
	loop) echo "$Z/build-modem" ;;
	net)  echo "$Z/build-modem-net" ;;
	*) echo "mode must be loop or net" >&2; exit 1 ;;
	esac
}

case "${1:-}" in
build)
	B=$(mode_dir "${2:-loop}")
	extra=()
	[ "${2:-loop}" = net ] && extra=(-- -DEXTRA_CONF_FILE="$APP/net.conf")
	cd "$Z"
	. .venv/bin/activate
	export ZEPHYR_SDK_INSTALL_DIR=$Z/zephyr-sdk-1.0.1 ZEPHYR_BASE=$Z/zephyr
	west build -p always -b qemu_x86_64 -d "$B" "$APP" "${extra[@]}"
	ninja -C "$B" zephyr/qemu_locore_image_target zephyr/qemu_main_image_target
	size "$B/zephyr/zephyr.elf"
	;;
run)
	B=$(mode_dir "${2:-loop}")
	: > "$LOG"
	timeout "${3:-0}" qemu-system-x86_64 -name "$NAME" -m 32 \
		-cpu qemu64,+x2apic,mmx,mmxext,sse,sse2 -machine q35 -machine acpi=off \
		-net none -no-reboot -nographic -monitor none -serial file:"$LOG" \
		-device ivshmem-doorbell,vectors=5,chardev=ivs \
		-chardev socket,path="$W/run/ivshmem.sock",id=ivs \
		-device loader,file="$B/zephyr/zephyr-qemu-main.elf" -smp cpus=2 \
		-kernel "$B/zephyr/zephyr-qemu-locore.elf"
	;;
stop)
	# SIGKILL: an abrupt "modem crash", nothing is published on the way out
	pkill -KILL -f -- "-name $NAME" || echo "not running"
	;;
status)
	pgrep -af -- "-name $NAME" || echo "not running"
	;;
*)
	echo "usage: $0 build [loop|net] | run [loop|net] [seconds] | stop | status" >&2
	exit 1
	;;
esac
