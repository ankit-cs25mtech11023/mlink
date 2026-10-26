#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P5.2: driver fault knobs on the AP TX path under TCP. For each fault:
# 15 s iperf3 AP->modem + ping, then fault off and a health check
# (100 pings must all come back). Output: bench/results/p5-faults-<kernel>.txt
#
# Usage (Git Bash): bash bench/run_p5_faults.sh
set -uo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
D=/sys/kernel/debug/mlink
KREL=$(ssh mlink-ap uname -r)
OUT="$REPO/bench/results/p5-faults-${KREL##*-}.txt"
g() { ssh -o BatchMode=yes "mlink-$1" "${@:2}"; }

for vm in ap modem; do g $vm "bash /mnt/share/mlink-down.sh >/dev/null; bash /mnt/share/mlink-up.sh $vm >/dev/null"; done
sleep 2
g modem "pkill iperf3; iperf3 -s -D"

run() {	# run <knob> <value> <label>
	echo "== $3 ($1=$2)"
	g ap "echo $2 > $D/$1"
	g ap "ping -i 0.02 -w 15 -q 10.77.0.2 | tail -2 | head -1" &
	echo "tcp: $(g ap "iperf3 -c 10.77.0.2 -t 15 -f m" | awk '/sender/ {print $7, $8, "retr", $9}')"
	wait
	g ap "echo 0 > $D/$1"
	echo "ap:    $(g ap "ethtool -S mlink0 | grep -E 'fault_|tx_kicks:' | tr -s ' ' | tr '\n' ' '")"
	echo "modem: $(g modem "ethtool -S mlink0 | grep -E 'rx_len_errors|rx_stalls|ring_corr|link_resets' | tr -s ' ' | tr '\n' ' '")"
	echo "after fault off: $(g ap "ping -c 100 -i 0.01 -q 10.77.0.2 | grep -o '[0-9.]*% packet loss'")"
}

{
	echo "# P5.2 fault injection — kernel $KREL ($( [[ $KREL == *debug* ]] && echo DEBUG || echo perf)), $(date -Iseconds)"
	echo "# command: bash bench/run_p5_faults.sh ; faults injected on the AP TX path, counters cumulative"
	run fault_drop_kick 10 "drop every 10th DATA kick"
	run fault_drop_kick 2 "drop every 2nd DATA kick"
	run fault_delay_kick_us 200 "delay every DATA kick 200 us"
	run fault_corrupt_tx_len 50 "invalid len on every 50th packet"
	echo "== dmesg gate"
	for vm in ap modem; do g $vm "bash /mnt/share/dmesg-gate.sh"; done
} 2>&1 | tee "$OUT"
