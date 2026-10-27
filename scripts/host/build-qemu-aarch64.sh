#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# P9 (D21): build qemu-system-aarch64 from the GPG-verified QEMU 10.2.2 source
# (P0.5) into ~/D/mlink-work/opt, without touching the system's QEMU packages.
# Run under thermal-run.sh in tmux.  JOBS=4 by default.
set -euo pipefail
W=$HOME/D/mlink-work
SRC=$W/src/qemu-10.2.2
B=$W/build/qemu-aarch64
P=$W/opt/qemu-aarch64

mkdir -p "$B" "$P"
cd "$B"
"$SRC/configure" --prefix="$P" --target-list=aarch64-softmmu \
	--disable-docs --disable-werror --disable-gtk --disable-sdl \
	--disable-opengl --disable-vnc
make -j"${JOBS:-4}"
make install
"$P/bin/qemu-system-aarch64" --version
