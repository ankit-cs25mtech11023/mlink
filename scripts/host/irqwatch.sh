#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Debug helper: every 2 s record a guest's cumulative mlink interrupt counts,
# NET_RX softirq count and soft-lockup count, keeping the last 300 samples, so
# the rates just before a lockup can be reconstructed afterwards.
#
# Usage: bash ~/D/mlink/scripts/host/irqwatch.sh modem   (in tmux)
set -uo pipefail

VM=${1:-modem}
PORT=2222
[ "$VM" = ap ] && PORT=2221
LOG="$HOME/D/mlink-work/logs/irqwatch-$VM.log"
: > "$LOG"

# Runs inside the guest; no nested quoting needed.
read -r -d '' PROBE <<'EOF'
d=$(awk '/mlink-data/ {print $2 + $3}' /proc/interrupts)
s=$(awk '/mlink-space/ {print $2 + $3}' /proc/interrupts)
n=$(awk '/NET_RX/ {print $2 + $3}' /proc/softirqs)
l=$(dmesg | grep -c "soft lockup")
echo "$(date +%T) data=$d space=$s net_rx=$n lockups=$l"
EOF

while :; do
	ssh -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=no \
	    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -p $PORT root@127.0.0.1 \
	    "bash -s" <<< "$PROBE" >> "$LOG" 2>&1 || echo "$(date +%T) ssh failed" >> "$LOG"
	tail -n 300 "$LOG" > "$LOG.tmp" && mv "$LOG.tmp" "$LOG"
	sleep 2
done
