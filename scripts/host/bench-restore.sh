#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Undo bench-prep.sh: restore governor/turbo, unpin the VMs, restart the
# Docker containers. Firefox is left for the user to reopen.
#
# Usage: bash ~/D/mlink/scripts/host/bench-restore.sh
set -uo pipefail

W="$HOME/D/mlink-work"
log() { echo "$(date -Is) bench-restore: $*" | tee -a "$W/logs/bg-apps.log"; }

log "temp after benchmarks: $(( $(cat /sys/class/thermal/thermal_zone5/temp) / 1000 ))C"
sudo -n /usr/local/sbin/mlink-bench-prep off >/dev/null && log "governor/turbo restored"
for vm in ap modem; do
	pid=$(cat "$W/run/$vm.pid" 2>/dev/null) && taskset -a -cp 0-7 "$pid" >/dev/null &&
		log "unpinned $vm"
done
docker start dev-api-1 dev-db-1 >/dev/null && log "started docker dev-api-1 dev-db-1"
