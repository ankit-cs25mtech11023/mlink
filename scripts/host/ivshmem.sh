#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Start/stop the ivshmem-server that backs both VMs' ivshmem-doorbell devices.
# It must be running before the VMs start; restarting it requires restarting
# both VMs (they lose their eventfds).
#
# Usage: bash ~/D/mlink/scripts/host/ivshmem.sh start|stop|status
set -euo pipefail

W="$HOME/D/mlink-work"
RUN="$W/run"
SOCK="$RUN/ivshmem.sock"
PID="$RUN/ivshmem.pid"
SHM=mlink-shm
SIZE=4M
VECTORS=5	# DATA SPACE STATE TEST CTRL

running() { [ -f "$PID" ] && kill -0 "$(cat "$PID")" 2>/dev/null; }

case "${1:-}" in
start)
	if running; then
		echo "ivshmem-server already running (pid $(cat "$PID"))"
		exit 0
	fi
	mkdir -p "$RUN" "$W/logs"
	rm -f "$SOCK"
	# The server refuses -v in daemon mode, and its verbose peer connect/
	# disconnect log is worth having, so run it -F and detach it ourselves.
	setsid "$W/bin/ivshmem-server" -F -v -S "$SOCK" -M "$SHM" -l "$SIZE" \
		-n "$VECTORS" >> "$W/logs/ivshmem-server.log" 2>&1 < /dev/null &
	echo $! > "$PID"
	for _ in 1 2 3 4 5 6 7 8 9 10; do
		[ -S "$SOCK" ] && running && break
		sleep 0.2
	done
	running || { echo "ivshmem-server failed to start" >&2; exit 1; }
	echo "ivshmem-server pid $(cat "$PID"), socket $SOCK, shm /dev/shm/$SHM"
	;;
stop)
	if running; then
		kill "$(cat "$PID")"
		echo "ivshmem-server stopped"
	else
		echo "ivshmem-server not running"
	fi
	rm -f "$PID" "$SOCK"
	;;
status)
	if running; then
		echo "running pid $(cat "$PID")"
		ls -l "$SOCK" "/dev/shm/$SHM"
	else
		echo "not running"
		exit 1
	fi
	;;
*)
	echo "usage: ivshmem.sh start|stop|status" >&2
	exit 2
	;;
esac
