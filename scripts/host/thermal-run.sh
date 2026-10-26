#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Run a long job (kernel build, soak) under a thermal guard. The laptop's B0D4
# zone powers the machine off at 98 C, and a remote user cannot power it back
# on. The job runs in its own process group; above HOT the whole group is
# frozen with SIGSTOP, below COOL it continues with SIGCONT.
#
# Usage: bash ~/D/mlink/scripts/host/thermal-run.sh <command> [args...]
#   HOT=88 COOL=78 (degrees C), LOG=... (transition log)
set -uo pipefail

HOT=${HOT:-88}
COOL=${COOL:-78}
LOG=${LOG:-$HOME/D/mlink-work/logs/thermal.log}

# Hottest of the CPU-related zones, in whole degrees.
temp() {
	local z t max=0
	for z in /sys/class/thermal/thermal_zone*; do
		case "$(cat "$z/type")" in
		B0D4|x86_pkg_temp|acpitz)
			t=$(( $(cat "$z/temp") / 1000 ))
			[ "$t" -gt "$max" ] && max=$t
			;;
		esac
	done
	echo "$max"
}

log() { echo "$(date -Is) $*" | tee -a "$LOG"; }

setsid "$@" &
PG=$!
log "start pgid=$PG hot=$HOT cool=$COOL temp=$(temp)C: $*"
# If we are killed, never leave the job frozen forever.
trap 'kill -CONT -- -$PG 2>/dev/null' EXIT

frozen=0
paused_s=0
while kill -0 "$PG" 2>/dev/null; do
	t=$(temp)
	if [ $frozen -eq 0 ] && [ "$t" -ge "$HOT" ]; then
		kill -STOP -- -"$PG" && frozen=1 && log "FREEZE at ${t}C"
	elif [ $frozen -eq 1 ] && [ "$t" -le "$COOL" ]; then
		kill -CONT -- -"$PG" && frozen=0 && log "resume at ${t}C"
	fi
	[ $frozen -eq 1 ] && paused_s=$((paused_s + 1))
	sleep 1
done
wait "$PG"
rc=$?
log "end rc=$rc, frozen for ${paused_s}s total, temp=$(temp)C"
exit $rc
