#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P5.5 hypothesis test: the peer (or shm corruption of want_kick) can drive a
# DATA-doorbell storm at the modem while its NAPI is polling. Since v6.4 net
# softirqs run again at every hardirq exit, so a storm plus expensive per-packet
# work (KASAN) can keep a CPU in irq/softirq context -> soft lockup.
# Runs iperf3 AP->modem for DUR s, without and with a doorbell storm from the AP,
# and reports modem DATA irqs, NET_RX softirqs, iperf3 rate and lockups.
# Output: bench/results/p5-storm-<kernel>.txt
#
# Usage: bash bench/run_p5_storm.sh [DUR] [STORMERS]
set -euo pipefail
DUR=${1:-60}
N=${2:-2}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
KREL=$(ssh mlink-modem uname -r)
OUT="$REPO/bench/results/p5-storm-${KREL##*-}.txt"
D=/sys/kernel/debug/mlink

data_irqs() { ssh mlink-modem "awk '/mlink.*data|mlink-data|mlink.*-0\$/ {s=0; for (i=2;i<=NF;i++) if (\$i ~ /^[0-9]+\$/) s+=\$i; print s}' /proc/interrupts | head -1"; }
net_rx() { ssh mlink-modem "awk '/NET_RX/ {s=0; for (i=2;i<=NF;i++) s+=\$i; print s}' /proc/softirqs"; }

run() {
	local storm=$1 i0 r0 i1 r1 mbit
	ssh mlink-modem 'dmesg -C; pkill -x iperf3; (iperf3 -s -D >/dev/null 2>&1)'
	sleep 1
	i0=$(data_irqs); r0=$(net_rx)
	if [ "$storm" = 1 ]; then
		for _ in $(seq 1 "$N"); do
			ssh mlink-ap "(timeout $DUR bash -c 'exec 3>$D/ring_doorbell; while :; do echo 0 >&3; done' >/dev/null 2>&1 &)"
		done
	fi
	mbit=$(ssh mlink-ap "iperf3 -c 10.77.0.2 -t $DUR -f m 2>&1 | awk '/receiver/ {print \$7}'")
	sleep 2
	i1=$(data_irqs); r1=$(net_rx)
	echo "storm=$storm: iperf3 AP->modem ${mbit:-?} Mbit/s, modem DATA irqs/s $(( (i1 - i0) / DUR )), NET_RX softirqs/s $(( (r1 - r0) / DUR ))"
	echo "  modem lockups/warnings: $(ssh mlink-modem 'dmesg | grep -ciE "soft lockup|hung task|rcu.*stall|BUG:"')"
	ssh mlink-modem 'dmesg | grep -iE "soft lockup|rcu.*stall" | head -3'
}

{
	echo "# P5.5 doorbell-storm test — modem kernel $KREL — $(date -Iseconds)"
	echo "# command: bash bench/run_p5_storm.sh $DUR $N   (storm = $N AP shells writing DATA to ring_doorbell)"
	echo "# modem DATA irq line: $(ssh mlink-modem "grep -m1 mlink /proc/interrupts | sed 's/  */ /g'")"
	run 0
	run 1
} 2>&1 | tee "$OUT"
