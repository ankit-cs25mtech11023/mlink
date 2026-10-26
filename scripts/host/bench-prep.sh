#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P6.2 benchmark hygiene (laptop). Stops the background load the user allowed
# us to stop (Firefox, Docker dev-api-1/dev-db-1 incl. its uvicorn), sets the
# governor to performance with turbo OFF (fixed 1.6 GHz: no thermal throttling,
# low run-to-run variance), drops caches, and pins each VM to its own two
# physical cores (CPU n and n+4 are hyperthread siblings on this i5-8250U):
#   ap -> CPUs 0,1     modem -> CPUs 2,3     host noise -> 4-7 (siblings)
# Undo with bench-restore.sh. Every action is logged in logs/bg-apps.log.
#
# Usage: bash ~/D/mlink/scripts/host/bench-prep.sh
set -uo pipefail

W="$HOME/D/mlink-work"
log() { echo "$(date -Is) bench-prep: $*" | tee -a "$W/logs/bg-apps.log"; }

pkill -x firefox-bin 2>/dev/null && log "stopped Firefox"
for c in dev-api-1 dev-db-1; do
	if docker ps --format '{{.Names}}' | grep -qx "$c"; then
		docker stop "$c" >/dev/null && log "stopped docker $c"
	fi
done
sudo -n /usr/local/sbin/mlink-bench-prep on >/dev/null && log "governor performance, turbo off"
sudo -n /usr/local/sbin/mlink-bench-prep drop-caches >/dev/null && log "dropped caches"

pin() {	# pin <vm> <cpulist>: every thread of that QEMU process
	local pid
	pid=$(cat "$W/run/$1.pid" 2>/dev/null) || { log "no pidfile for $1"; return; }
	taskset -a -cp "$2" "$pid" >/dev/null && log "pinned $1 (pid $pid) to CPUs $2"
}
pin ap 0,1
pin modem 2,3

log "temp before benchmarks: $(( $(cat /sys/class/thermal/thermal_zone5/temp) / 1000 ))C"
sudo -n /usr/local/sbin/mlink-bench-prep status
