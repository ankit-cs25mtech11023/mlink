#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Mirror the Windows repo (source of truth) to the laptop at ~/D/mlink.
# The mirror is disposable: it is wiped and re-extracted on every sync, but only
# after the remote side proves the target is our mirror (see guard below).
#
# Usage (Git Bash, from anywhere): bash scripts/win/sync.sh
set -euo pipefail

HOST=${MLINK_HOST:-kali}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$REPO"

# CRLF in a shell script or Kbuild file breaks it in confusing ways; refuse early.
bad=$(git ls-files -co --exclude-standard --eol | awk '$2 ~ /crlf/ {print $NF}')
if [ -n "$bad" ]; then
	echo "sync: refusing, CRLF line endings in:" >&2
	echo "$bad" >&2
	exit 1
fi

# Runs on the laptop. Kali's login shell is zsh, so we ship it base64-encoded and
# run it under bash explicitly.
read -r -d '' REMOTE <<'EOF' || true
set -euo pipefail
T="$HOME/D/mlink"
[ -n "$HOME" ] && [ -d "$HOME/D" ] || { echo "sync: ~/D missing" >&2; exit 1; }
if [ -L "$T" ]; then
	echo "sync: refusing, $T is a symlink" >&2; exit 1
fi
if [ -e "$T" ]; then
	[ -d "$T" ] || { echo "sync: refusing, $T is not a directory" >&2; exit 1; }
	[ "$(realpath "$T")" = "$HOME/D/mlink" ] ||
		{ echo "sync: refusing, $T does not resolve to itself" >&2; exit 1; }
	[ -f "$T/.mlink-mirror" ] ||
		{ echo "sync: refusing, $T has no .mlink-mirror marker" >&2; exit 1; }
	find "$T" -mindepth 1 -delete
else
	mkdir "$T"
fi
touch "$T/.mlink-mirror"
tar -xf - -C "$T"
echo "sync: $(find "$T" -type f | wc -l) files in $T"
EOF
B64=$(printf '%s' "$REMOTE" | base64 -w0)

# Tracked + new untracked files, honouring .gitignore; never ship .git itself.
# Skip files deleted in the working tree but still in the index.
git ls-files -co --exclude-standard -z |
	while IFS= read -r -d '' f; do
		# 'if', not '&&': a false test on the last file must not fail the pipe
		if [ -e "$f" ]; then printf '%s\0' "$f"; fi
	done |
	tar --null -T - -cf - |
	ssh -o BatchMode=yes "$HOST" "bash -c \"\$(echo $B64 | base64 -d)\""
