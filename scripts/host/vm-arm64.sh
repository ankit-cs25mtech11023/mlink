#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P9 (D21): boot the arm64 guest (TCG) as the modem peer on the mlink
# ivshmem-server. Control shell: `nc 127.0.0.1 2223` on the laptop (bound to
# the loopback only). Serial log: logs/arm64-serial.log. Run "run" in tmux.
#
# Usage: vm-arm64.sh run [extra kernel cmdline] | stop | status | sh '<commands>'
set -euo pipefail
W="$HOME/D/mlink-work"
QEMU="$W/opt/qemu-aarch64/bin/qemu-system-aarch64"
NAME=mlink-arm64

case "${1:-}" in
run)
	exec "$QEMU" -name "$NAME" -M virt,gic-version=3 -cpu max -smp 2 -m 512 \
		-accel tcg,thread=multi -nographic -monitor none \
		-serial file:"$W/logs/arm64-serial.log" \
		-kernel "$W/build/arm64/arch/arm64/boot/Image" \
		-initrd "$W/images/arm64-initramfs.cpio.gz" \
		-append "console=ttyAMA0 panic=-1 ${2:-}" -no-reboot \
		-device ivshmem-doorbell,vectors=5,chardev=ivs \
		-chardev socket,path="$W/run/ivshmem.sock",id=ivs \
		-netdev user,id=n0,hostfwd=tcp:127.0.0.1:2223-:23 \
		-device virtio-net-pci,netdev=n0
	;;
stop)
	pkill -KILL -f -- "-name $NAME" || echo "not running"
	;;
status)
	pgrep -af -- "^$QEMU -name $NAME" || echo "not running"
	;;
sh)
	# One command batch through the control shell; output comes back on stdout.
	printf '%s\nexit\n' "${2:?commands}" | timeout "${TMO:-60}" nc 127.0.0.1 2223
	;;
*)
	echo "usage: $0 run [cmdline] | stop | status | sh '<commands>'" >&2
	exit 2
	;;
esac
