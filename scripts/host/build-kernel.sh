#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Configure and build a guest kernel flavour out of tree.
#   flavour: debug | perf | kcsan
# Run it in tmux; it takes 30-90 min on the i5-8250U for a first debug build.
#
# Usage: bash ~/D/mlink/scripts/host/build-kernel.sh <flavour> [config-only]
set -euo pipefail

FLAVOUR=${1:?usage: build-kernel.sh debug|perf|kcsan [config-only]}
case "$FLAVOUR" in debug|perf|kcsan) ;; *) echo "bad flavour" >&2; exit 2 ;; esac

W="$HOME/D/mlink-work"
K="$W/src/linux"
B="$W/build/$FLAVOUR"
CFG="$HOME/D/mlink/configs"
JOBS=${JOBS:-6}

mkdir -p "$B"
cd "$K"

make O="$B" x86_64_defconfig kvm_guest.config
# perf adds nothing over base, but the fragment sets its LOCALVERSION.
scripts/kconfig/merge_config.sh -m -O "$B" "$B/.config" \
	"$CFG/mlink-base.config" "$CFG/$FLAVOUR.config"
make O="$B" olddefconfig

# merge_config silently drops options whose dependencies are missing, so check
# that every requested option actually landed. Later fragments win.
declare -A want=()
for f in "$CFG/mlink-base.config" "$CFG/$FLAVOUR.config"; do
	while IFS= read -r l; do
		if [[ $l =~ ^(CONFIG_[A-Za-z0-9_]+)=(.*)$ ]]; then
			want[${BASH_REMATCH[1]}]=${BASH_REMATCH[2]}
		elif [[ $l =~ ^#\ (CONFIG_[A-Za-z0-9_]+)\ is\ not\ set$ ]]; then
			want[${BASH_REMATCH[1]}]=n
		fi
	done < "$f"
done
bad=0
for k in "${!want[@]}"; do
	v=${want[$k]}
	if [ "$v" = n ]; then
		grep -q "^$k=" "$B/.config" && { echo "CONFIG MISMATCH: $k should be unset, is $(grep "^$k=" "$B/.config")"; bad=1; }
	else
		grep -qx "$k=$v" "$B/.config" || { echo "CONFIG MISMATCH: $k=$v, got: $(grep -E "^(# )?$k[= ]" "$B/.config" || echo absent)"; bad=1; }
	fi
done
[ $bad -eq 0 ] && echo "config check: all ${#want[@]} requested options present"

[ "${2:-}" = config-only ] && exit 0

start=$(date +%s)
nice -n 10 make O="$B" -j"$JOBS" CC="ccache gcc" bzImage modules
echo "build $FLAVOUR done in $(( ($(date +%s) - start) / 60 )) min: $(make -s O="$B" kernelrelease)"
ls -l "$B/arch/x86/boot/bzImage"
