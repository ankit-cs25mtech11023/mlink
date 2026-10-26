#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P0.6: build only contrib/ivshmem-server from the QEMU source (Debian/Kali do
# not package it) and install it to ~/D/mlink-work/bin. No root needed.
#
# Usage: bash ~/D/mlink/scripts/host/build-ivshmem-server.sh
set -euo pipefail

QVER=${QVER:-10.2.2}
W="$HOME/D/mlink-work"
Q="$W/src/qemu-$QVER"

cd "$Q"
# ivshmem-server is gated on have_tools && CONFIG_EVENTFD in meson.build; we only
# need one system target so that configure takes the "system + tools" path.
if [ ! -f build/build.ninja ]; then
	./configure --target-list=x86_64-softmmu --enable-tools \
		--disable-docs --disable-werror
fi
ninja -C build contrib/ivshmem-server/ivshmem-server
install -m 0755 build/contrib/ivshmem-server/ivshmem-server "$W/bin/ivshmem-server"
"$W/bin/ivshmem-server" -h | head -30
echo "installed: $W/bin/ivshmem-server"
