#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Sync -> build mlink.ko on the laptop against the guest kernel -> stage the
# module and guest scripts into the 9p share (/mnt/share in both VMs).
#
# Usage (Git Bash): bash scripts/win/rbuild.sh      KFLAVOUR=perf for the perf kernel
set -euo pipefail

HOST=${MLINK_HOST:-kali}
KFLAVOUR=${KFLAVOUR:-debug}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

bash "$REPO/scripts/win/sync.sh"
ssh -o BatchMode=yes "$HOST" bash -s -- "$KFLAVOUR" <<'EOF'
set -euo pipefail
F=$1
S="$HOME/D/mlink-work/share"
make -C "$HOME/D/mlink/driver" KFLAVOUR="$F"
cp "$HOME/D/mlink/driver/mlink.ko" "$S/mlink.ko"
cp "$HOME/D/mlink/scripts/guest/"*.sh "$HOME/D/mlink/tools/"*.py "$S/"
echo "staged: $(modinfo -F vermagic "$S/mlink.ko")"
EOF
