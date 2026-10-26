#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Smoke test, run after every driver change. Phase 0 skeleton:
# build + stage -> load/unload in both guests -> dmesg gate -> PASS/FAIL.
# P3.9 extends it with link-up, ping and iperf3.
#
# Usage (Git Bash): bash scripts/win/test.sh
set -uo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MARK="mlink-test-$(date +%s)"
fail=0

bash "$REPO/scripts/win/rbuild.sh" || { echo "TEST: FAIL (build)"; exit 1; }

for vm in ap modem; do
	echo "--- $vm"
	if ! ssh -o BatchMode=yes -o ConnectTimeout=10 "mlink-$vm" bash -s -- "$MARK" <<'EOF'
set -euo pipefail
echo "$1" > /dev/kmsg
lsmod | grep -q '^mlink ' && rmmod mlink
insmod /mnt/share/mlink.ko
lsmod | grep '^mlink '
rmmod mlink
dmesg | grep 'mlink:' | tail -5
bash /mnt/share/dmesg-gate.sh "$1"
EOF
	then
		fail=1
	fi
done

if [ $fail -eq 0 ]; then echo "TEST: PASS"; else echo "TEST: FAIL"; exit 1; fi
