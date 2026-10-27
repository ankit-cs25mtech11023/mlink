#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P9.2: x86 AP (KVM, mlink-ap) <-> arm64 modem (TCG, vm-arm64.sh) on one
# ivshmem region. Ring selftest both directions, ping, reload loop, SSR.
# Prereq: arm64 guest running (tmux "arm64"), AP mlink loaded, x86 modem unloaded.
# Output: bench/results/p9-arm64.txt
#
# Usage: bash bench/run_p9_arm64.sh [N]
set -euo pipefail

N=${1:-1000000}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT="$REPO/bench/results/p9-arm64.txt"
D=/sys/kernel/debug/mlink
P=/sys/module/mlink/parameters
VM='bash ~/D/mlink/scripts/host/vm-arm64.sh'

ap() { ssh mlink-ap "$1"; }
# arm64 guest shell (no echo, no prompt); drops the trailing rc line
arm() { ssh kali "$VM sh '$1' ${2:-120}" | tr -d '\r' | sed -e '/^rc=[0-9]* *$/d' -e '/^$/d' || true; }
wait_up() { for _ in $(seq 1 60); do ap 'cat /sys/class/net/mlink0/carrier' 2>/dev/null | grep -q 1 && return 0; sleep 1; done; return 1; }

{
	echo "# P9.2 x86 AP <-> arm64 modem — $(date -Iseconds)"
	echo "# AP: $(ap 'uname -r') (KVM)   modem: $(arm 'uname -r -m' | tr -d '\r') (TCG, -cpu max, MTTCG, 2 vCPU)"
	echo "# command: bash bench/run_p9_arm64.sh $N"

	# The selftest owns TX only while mlink0 is down (one producer per ring).
	ap 'ip link set mlink0 down'
	arm 'ip link set mlink0 down'
	for dir in "ap arm" "arm ap"; do
		set -- $dir
		echo "== ring selftest $1 -> $2, N=$N, kick_suppress=1"
		if [ "$1" = ap ]; then
			ap "echo $N > $D/selftest"; sleep 2
			echo "producer (x86):   $(ap "cat $D/selftest")"
			echo "consumer (arm64): $(arm "cat $D/selftest_rx" | tr -d '\r')"
		else
			arm "echo $N > $D/selftest" 900 >/dev/null; sleep 2
			echo "producer (arm64): $(arm "cat $D/selftest" | tr -d '\r')"
			echo "consumer (x86):   $(ap "cat $D/selftest_rx")"
		fi
	done
	echo "AP stats: $(ap "grep -E 'rx_stalls|ring_corruptions|link_resets' $D/stats | tr '\n' ' '")"
	ap 'ip link set mlink0 up'
	arm 'ip link set mlink0 up'
	wait_up || echo "carrier did not come back"

	echo "== ping x86 -> arm64"
	ap 'ping -c 50 -i 0.05 10.77.0.2 | tail -2'
	ap 'ping -c 20 -s 3000 -i 0.1 10.77.0.2 | tail -1'
	echo "== ping arm64 -> x86"
	arm 'ping -c 20 -i 0.2 10.77.0.1 | tail -2' | tr -d '\r'
	echo "== flood x86 -> arm64 (nstat deltas)"
	ap 'nstat -n; ping -f -l 64 -c 5000 -q -w 60 10.77.0.2 | tail -2 | head -1; sleep 2; nstat | grep -E "IcmpOutEchos|IcmpInEchoReps"'

	echo "== arm64 rmmod/insmod x20 (AP must re-handshake each time)"
	ups0=$(ap "grep -E '^link_ups' $D/stats | awk '{print \$2}'")
	arm 'K=/lib/modules/$(uname -r)/mlink.ko; for i in $(seq 1 20); do rmmod mlink || exit 1; insmod $K role=modem || exit 1; sleep 1; done; echo reload-loop ok' 900 | tr -d '\r'
	arm 'ip addr add 10.77.0.2 peer 10.77.0.1 dev mlink0; ip link set mlink0 up' >/dev/null
	wait_up && echo "AP link_ups +$(( $(ap "grep -E '^link_ups' $D/stats | awk '{print \$2}'") - ups0 ))"
	ap 'ping -c 5 -i 0.2 10.77.0.2 | tail -2 | head -1'

	echo "== SSR: kill the arm64 VM (no restart), AP detection"
	ssh kali "$VM stop" >/dev/null
	sleep 3
	ap "grep -E 'last_hb_age_ms|link_resets' $D/link; dmesg | grep 'link reset' | tail -1"
	echo "== restart arm64 VM"
	ssh kali "tmux kill-session -t arm64 2>/dev/null; tmux new-session -d -s arm64 '$VM run'"
	wait_up && ap "dmesg | grep 'link UP' | tail -1"
	ap 'ping -c 5 -i 0.2 10.77.0.2 | tail -2 | head -1'

	echo "== arm64 dmesg warnings/oopses"
	arm 'dmesg | grep -ciE "warn|bug|oops|lockup"' | tr -d '\r'
} 2>&1 | tee "$OUT"
