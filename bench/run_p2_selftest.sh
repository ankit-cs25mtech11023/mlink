#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P2.3: ring selftest both directions, kick suppression on/off.
# Runs from Git Bash on Windows; both guests must have mlink loaded.
# Output: bench/results/p2-selftest-<kernel>.txt
#
# Usage: bash bench/run_p2_selftest.sh [N]
set -euo pipefail

N=${1:-10000000}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
D=/sys/kernel/debug/mlink
P=/sys/module/mlink/parameters
KREL=$(ssh mlink-ap uname -r)
OUT="$REPO/bench/results/p2-selftest-${KREL##*-}.txt"

{
	echo "# P2.3 ring selftest — kernel $KREL ($( [[ $KREL == *debug* ]] && echo 'DEBUG: KASAN+lockdep, not a headline number' || echo perf))"
	echo "# date: $(date -Iseconds)"
	echo "# host: i5-8250U, $(ssh kali 'sudo -n /usr/local/sbin/mlink-bench-prep status' | tr '\n' ' ')"
	echo "# command: bash bench/run_p2_selftest.sh $N"
	echo "# producer: echo N > $D/selftest ; consumer verifies seq/len/xorshift payload"
	for dir in "ap modem" "modem ap"; do
		set -- $dir
		for ks in 1 0; do
			ssh "mlink-$1" "echo $ks > $P/kick_suppress"
			ssh "mlink-$1" "echo $N > $D/selftest"
			sleep 1
			echo "== $1 -> $2 kick_suppress=$ks"
			echo "producer: $(ssh "mlink-$1" "cat $D/selftest")"
			echo "consumer: $(ssh "mlink-$2" "cat $D/selftest_rx")"
			echo "consumer: rx_stalls=$(ssh "mlink-$2" "grep rx_stalls $D/stats | awk '{print \$2}'") ring_corruptions=$(ssh "mlink-$2" "grep ring_corruptions $D/stats | awk '{print \$2}'")"
			echo "temp: $(ssh kali 'echo $(( $(cat /sys/class/thermal/thermal_zone5/temp)/1000 ))C')"
		done
		ssh "mlink-$1" "echo 1 > $P/kick_suppress"
	done
} | tee "$OUT"
