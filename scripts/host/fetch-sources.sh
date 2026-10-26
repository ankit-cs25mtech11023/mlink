#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P0.5: fetch the guest kernel (LTS, shallow) and the QEMU source matching the
# host QEMU (only needed to build ivshmem-server). Idempotent; no root needed.
#
# Usage: bash ~/D/mlink/scripts/host/fetch-sources.sh
set -euo pipefail

KTAG=${KTAG:-v6.18.59}
QVER=${QVER:-10.2.2}
# QEMU release signing key (Michael Roth), see https://www.qemu.org/download/
QKEY=CEACC9E15534EBABB82D3FA03353C9CEF108B584

W="$HOME/D/mlink-work"
SRC="$W/src"
mkdir -p "$SRC"

# --- kernel ------------------------------------------------------------------
if [ -d "$SRC/linux/.git" ]; then
	echo "kernel: $(git -C "$SRC/linux" describe --tags) already present"
else
	git clone --depth 1 --branch "$KTAG" \
		https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git "$SRC/linux" ||
	git clone --depth 1 --branch "$KTAG" \
		https://github.com/gregkh/linux.git "$SRC/linux"
fi
[ "$(git -C "$SRC/linux" describe --tags)" = "$KTAG" ] ||
	{ echo "kernel: tag mismatch" >&2; exit 1; }

# --- QEMU --------------------------------------------------------------------
cd "$SRC"
T="qemu-$QVER.tar.xz"
[ -f "$T" ] || curl -fL -o "$T" "https://download.qemu.org/$T"
[ -f "$T.sig" ] || curl -fL -o "$T.sig" "https://download.qemu.org/$T.sig"

# Private keyring so the user's ~/.gnupg is never touched.
export GNUPGHOME="$W/gnupg"
mkdir -p "$GNUPGHOME"
chmod 700 "$GNUPGHOME"
gpg --batch --list-keys "$QKEY" >/dev/null 2>&1 ||
	gpg --batch --keyserver hkps://keyserver.ubuntu.com --recv-keys "$QKEY"
gpg --batch --status-fd 1 --verify "$T.sig" "$T" 2>/dev/null |
	grep -q "VALIDSIG $QKEY" || { echo "qemu: SIGNATURE CHECK FAILED" >&2; exit 1; }
echo "qemu: signature OK ($QKEY)"
sha256sum "$T"

[ -d "qemu-$QVER" ] || tar -xf "$T"
echo "done: kernel $KTAG in $SRC/linux, qemu in $SRC/qemu-$QVER"
