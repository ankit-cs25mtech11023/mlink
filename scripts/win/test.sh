#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Smoke test, run after every driver change (P3.9):
# build + stage -> (re)load + mlink0 up in both guests -> peer discovery ->
# doorbell ping-pong -> ping 100 -> iperf3 TCP 5 s -> counters -> unload ->
# dmesg gate -> PASS/FAIL.
#
# Usage (Git Bash): bash scripts/win/test.sh
set -uo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MARK="mlink-test-$(date +%s)"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10)
D=/sys/kernel/debug/mlink
fail=0

g() { "${SSH[@]}" "mlink-$1" "${@:2}"; }

bash "$REPO/scripts/win/rbuild.sh" >/dev/null || { echo "TEST: FAIL (build)"; exit 1; }
echo "build: ok"

for vm in ap modem; do
	g "$vm" "echo $MARK > /dev/kmsg; bash /mnt/share/mlink-down.sh >/dev/null; bash /mnt/share/mlink-up.sh $vm >/dev/null" ||
		{ echo "load $vm: FAIL"; fail=1; }
done

for vm in ap modem; do
	ok=0
	for _ in $(seq 1 30); do
		if g "$vm" "grep -q 'peer_found: 1' $D/info"; then ok=1; break; fi
		sleep 0.2
	done
	if [ $ok -eq 1 ]; then echo "peer $vm: ok"; else echo "peer $vm: FAIL"; fail=1; fi
done

pp=$(g ap "echo 200 > $D/pingpong && cat $D/pingpong")
echo "pingpong: $pp"
case "$pp" in *"count 200 lost 0"*) ;; *) fail=1 ;; esac

p=$(g ap "ping -c 100 -i 0.01 -q 10.77.0.2 | grep -o '[0-9]*% packet loss'")
echo "ping: $p"
[ "$p" = "0% packet loss" ] || fail=1

g modem "pkill iperf3; iperf3 -s -D -1" >/dev/null
sleep 0.5
bw=$(g ap "iperf3 -c 10.77.0.2 -t 5 -f m | grep receiver | awk '{print \$7, \$8}'")
echo "iperf3 TCP 5s: $bw"
[ -n "$bw" ] || fail=1

for vm in ap modem; do
	c=$(g "$vm" "ethtool -S mlink0 | grep -E 'rx_stalls|ring_corruptions|rx_len_errors' | awk '{s+=\$2} END {print s}'")
	echo "error counters $vm: $c"
	[ "$c" = 0 ] || fail=1
done

for vm in modem ap; do
	g "$vm" "bash /mnt/share/mlink-down.sh" >/dev/null || { echo "unload $vm: FAIL"; fail=1; }
done
for vm in ap modem; do
	g "$vm" "bash /mnt/share/dmesg-gate.sh $MARK" || fail=1
done

if [ $fail -eq 0 ]; then echo "TEST: PASS"; else echo "TEST: FAIL"; exit 1; fi
