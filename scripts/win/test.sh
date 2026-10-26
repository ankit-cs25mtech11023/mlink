#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Smoke test, run after every driver change:
# build + stage -> (re)load in both guests -> wait for peer discovery ->
# doorbell ping-pong -> unload -> dmesg gate -> PASS/FAIL.
# P3.9 extends it with link-up, ping and iperf3.
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
	g "$vm" "echo $MARK > /dev/kmsg; if lsmod | grep -q '^mlink '; then rmmod mlink; fi; insmod /mnt/share/mlink.ko role=$vm" ||
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

for vm in modem ap; do
	g "$vm" "rmmod mlink" || { echo "unload $vm: FAIL"; fail=1; }
done
for vm in ap modem; do
	g "$vm" "bash /mnt/share/dmesg-gate.sh $MARK" || fail=1
done

if [ $fail -eq 0 ]; then echo "TEST: PASS"; else echo "TEST: FAIL"; exit 1; fi
