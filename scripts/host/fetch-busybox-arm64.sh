#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P9 (D21): fetch busybox-static for arm64 as a normal user, without enabling
# a foreign architecture on the system. apt runs with a private state/cache
# under ~/D/mlink-work/opt/apt-arm64 but the system's sources and trusted keys,
# so the .deb is still checked against the signed archive index. The binary is
# unpacked into ~/D/mlink-work/opt/busybox-arm64/bin/busybox.
set -euo pipefail
W="$HOME/D/mlink-work"
A="$W/opt/apt-arm64"
mkdir -p "$A/lists/partial" "$A/cache/archives/partial"
: > "$A/status"
O=(-o Dir::State="$A" -o Dir::State::Lists="$A/lists" -o Dir::State::status="$A/status"
   -o Dir::Cache="$A/cache" -o Dir::Cache::pkgcache="$A/cache/pkgcache.bin"
   -o Dir::Cache::srcpkgcache="$A/cache/srcpkgcache.bin"
   -o APT::Architecture=arm64 -o APT::Architectures=arm64 -o Debug::NoLocking=1)

apt-get "${O[@]}" update
cd "$A"
rm -f busybox-static_*.deb
apt-get "${O[@]}" download busybox-static:arm64
deb=$(ls busybox-static_*_arm64.deb)
dpkg-deb -f "$deb" Package Version Architecture
rm -rf "$W/opt/busybox-arm64"
mkdir -p "$W/opt/busybox-arm64/root"
dpkg-deb -x "$deb" "$W/opt/busybox-arm64/root"
mkdir -p "$W/opt/busybox-arm64/bin"
bb=$(find "$W/opt/busybox-arm64/root" -name busybox -type f | head -1)
cp "$bb" "$W/opt/busybox-arm64/bin/busybox"
file "$W/opt/busybox-arm64/bin/busybox"
