#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# A/B throughput check on one boot: two mlink.ko builds, interleaved (order
# flipped every round) so drift hits both equally. Same iperf method as E2 in
# run_matrix.sh. Run ON THE LAPTOP in tmux after bench-prep (perf kernel).
#   A, B: paths to the two mlink.ko files; output CSV: build,round,mtu,dir,mbps,extra
#
# Usage: bash bench/run_ab_e2.sh A.ko B.ko OUT.csv [ROUNDS] [SECS]
set -uo pipefail

A=$1 B=$2 OUT=$3 ROUNDS=${4:-5} SECS=${5:-10}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SHARE="$HOME/D/mlink-work/share"
D=/sys/kernel/debug/mlink
g() {
	local port=2221
	[ "$1" = modem ] && port=2222
	ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
	    -o LogLevel=ERROR -p $port root@127.0.0.1 "${@:2}"
}
use() {	# stage one build as the shared mlink.ko, reload both sides, wait for UP
	cp "$1" "$SHARE/mlink.ko"
	for vm in ap modem; do g $vm "bash /mnt/share/mlink-down.sh >/dev/null; bash /mnt/share/mlink-up.sh $vm >/dev/null"; done
	for _ in $(seq 1 100); do
		g ap "grep -q 'state: *UP' $D/link" && g modem "grep -q 'state: *UP' $D/link" && break
		sleep 0.2
	done
	g modem "pkill iperf3; iperf3 -s -D"
	sleep 1
	g ap "iperf3 -c 10.77.0.2 -t 3" >/dev/null 2>&1	# warm-up, discarded
}
iperf() { g ap "iperf3 -c 10.77.0.2 -t $SECS -J $*" 2>/dev/null | python3 "$REPO/bench/iperf_json.py"; }

cp "$SHARE/mlink.ko" "$SHARE/mlink.ko.ab-saved"
echo "build,round,mtu,dir,mbps,extra" > "$OUT"
for r in $(seq 1 "$ROUNDS"); do
	order="A B"; [ $((r % 2)) -eq 0 ] && order="B A"
	for b in $order; do
		[ $b = A ] && use "$A" || use "$B"
		for cfg in "576 fwd" "576 rev" "1500 fwd" "4000 fwd" "4000 rev"; do
			set -- $cfg
			for vm in ap modem; do g $vm "ip link set mlink0 mtu $1"; done
			a=""; [ "$2" = rev ] && a="-R"
			res=$(iperf "$a")
			echo "$(date +%T) round $r build $b mtu $1 $2: $res"
			echo "$b,$r,$1,$2,$res" >> "$OUT"
		done
		for vm in ap modem; do g $vm "ip link set mlink0 mtu 1500"; done
	done
done
mv "$SHARE/mlink.ko.ab-saved" "$SHARE/mlink.ko"
echo "A/B done"
