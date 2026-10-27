#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P9 (D21): boot the arm64 guest (TCG) as the modem peer on the mlink
# ivshmem-server. The serial port is a unix socket (run/arm64-console.sock)
# carrying the control shell, held open by arm64-bridge.py; QEMU copies all of
# it to logs/arm64-serial.log. Use the "sh" subcommand. Run "run" in tmux.
#
# Usage: vm-arm64.sh run [extra kernel cmdline] | stop | status | sh '<commands>' [timeout_s]
set -euo pipefail
W="$HOME/D/mlink-work"
QEMU="$W/opt/qemu-aarch64/bin/qemu-system-aarch64"
NAME=mlink-arm64

case "${1:-}" in
run)
	: > "$W/logs/arm64-serial.log"
	# One bridge per host, holding the console connection across VM restarts.
	pgrep -f "arm64-bridge.py" >/dev/null ||
		setsid python3 "$HOME/D/mlink/scripts/host/arm64-bridge.py" \
			>>"$W/logs/arm64-bridge.log" 2>&1 < /dev/null &
	exec "$QEMU" -name "$NAME" -M virt,gic-version=3 -cpu max -smp 2 -m 512 \
		-accel tcg,thread=multi -nographic -monitor none \
		-chardev socket,id=ser,path="$W/run/arm64-console.sock",server=on,wait=off,logfile="$W/logs/arm64-serial.log" \
		-serial chardev:ser \
		-kernel "$W/build/arm64/arch/arm64/boot/Image" \
		-initrd "$W/images/arm64-initramfs.cpio.gz" \
		-append "console=ttyAMA0 loglevel=5 panic=-1 ${2:-}" -no-reboot \
		-device ivshmem-doorbell,vectors=5,chardev=ivs \
		-chardev socket,path="$W/run/ivshmem.sock",id=ivs
	;;
stop)
	pkill -KILL -f -- "^$QEMU -name $NAME" || echo "not running"
	;;
status)
	pgrep -af -- "^$QEMU -name $NAME" || echo "not running"
	;;
sh)
	python3 "$HOME/D/mlink/scripts/host/arm64-sh.py" "${2:?commands}" "${3:-60}"
	;;
*)
	echo "usage: $0 run [cmdline] | stop | status | sh '<commands>' [timeout_s]" >&2
	exit 2
	;;
esac
