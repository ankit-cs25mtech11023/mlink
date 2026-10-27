#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P9 (D21): arm64 guest pieces, all under ~/D/mlink-work, cross-built with the
# kernel.org nolibc GCC (opt/gcc-15.2.0-nolibc).
#   kernel     allnoconfig + configs/arm64-min.config -> build/arm64/arch/arm64/boot/Image
#   module     mlink.ko for that kernel, built in a copy of driver/ (build/mlink-arm64)
#   initramfs  busybox (opt/busybox-arm64) + mlink.ko + guest/arm64-init -> images/
#   all        the three in order
# Run "kernel" in tmux under thermal-run.sh.
#
# Usage: bash ~/D/mlink/scripts/host/build-arm64.sh kernel|module|initramfs|all
set -euo pipefail

W="$HOME/D/mlink-work"
K="$W/src/linux"
B="$W/build/arm64"
CFG="$HOME/D/mlink/configs/arm64-min.config"
X="$W/opt/gcc-15.2.0-nolibc/aarch64-linux/bin/aarch64-linux-"
KMAKE=(make ARCH=arm64 CROSS_COMPILE="$X")
JOBS=${JOBS:-4}

kernel() {
	mkdir -p "$B"
	cd "$K"
	"${KMAKE[@]}" O="$B" allnoconfig
	scripts/kconfig/merge_config.sh -m -O "$B" "$B/.config" "$CFG"
	"${KMAKE[@]}" O="$B" olddefconfig
	# merge_config drops options with unmet dependencies: check every one landed.
	local bad=0 k v l
	while IFS= read -r l; do
		if [[ $l =~ ^(CONFIG_[A-Za-z0-9_]+)=(.*)$ ]]; then
			k=${BASH_REMATCH[1]} v=${BASH_REMATCH[2]}
			grep -qx "$k=$v" "$B/.config" ||
				{ echo "CONFIG MISMATCH: $k=$v, got: $(grep -E "^(# )?$k[= ]" "$B/.config" || echo absent)"; bad=1; }
		elif [[ $l =~ ^#\ (CONFIG_[A-Za-z0-9_]+)\ is\ not\ set$ ]]; then
			k=${BASH_REMATCH[1]}
			grep -q "^$k=" "$B/.config" && { echo "CONFIG MISMATCH: $k should be unset"; bad=1; }
		fi
	done < "$CFG"
	[ $bad -eq 0 ] && echo "config check: all requested options present"
	local start; start=$(date +%s)
	# "modules" (even with none configured) runs modpost on vmlinux and writes
	# Module.symvers, which the out-of-tree mlink.ko needs to resolve exports.
	nice -n 10 "${KMAKE[@]}" O="$B" -j"$JOBS" Image modules
	echo "arm64 kernel done in $(( ($(date +%s) - start) / 60 )) min: $("${KMAKE[@]}" -s O="$B" kernelrelease)"
	ls -l "$B/arch/arm64/boot/Image"
}

module() {
	local M="$W/build/mlink-arm64"
	# A copy, so the x86 objects in driver/ (rbuild.sh) are never mixed with arm64 ones.
	rm -rf "$M"
	mkdir -p "$M"
	cp "$HOME/D/mlink/driver/"*.[ch] "$HOME/D/mlink/driver/Kbuild" "$M/"
	"${KMAKE[@]}" -C "$B" M="$M" modules
	file "$M/mlink.ko"
}

initramfs() {
	local R="$W/build/arm64-initramfs" BB="$W/opt/busybox-arm64/bin/busybox"
	local KREL; KREL=$("${KMAKE[@]}" -s -C "$K" O="$B" kernelrelease)
	rm -rf "$R"
	mkdir -p "$R"/{bin,sbin,dev,proc,sys,tmp,root,lib/modules/"$KREL"}
	cp "$BB" "$R/bin/busybox"
	# The arm64 binary cannot run here; /init installs the applet links at boot.
	ln -s busybox "$R/bin/sh"
	cp "$W/build/mlink-arm64/mlink.ko" "$R/lib/modules/$KREL/"
	cp "$HOME/D/mlink/scripts/guest/arm64-init" "$R/init"
	chmod 755 "$R/init"
	mkdir -p "$W/images"
	(cd "$R" && find . | cpio -o -H newc --quiet | gzip -9) > "$W/images/arm64-initramfs.cpio.gz"
	ls -l "$W/images/arm64-initramfs.cpio.gz"
}

case "${1:-}" in
kernel) kernel ;;
module) module ;;
initramfs) initramfs ;;
all) kernel; module; initramfs ;;
*) echo "usage: $0 kernel|module|initramfs|all" >&2; exit 2 ;;
esac
