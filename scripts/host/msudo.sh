#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Run one command as root on the laptop without the password ever appearing on a
# command line, in output, or in Claude's context (plan.md D13, CLAUDE.md rule 3).
# sudo -A asks the askpass helper, which reads the user-written password file.
#
# Usage: bash ~/D/mlink/scripts/host/msudo.sh <command> [args...]
set -euo pipefail

SECRETS="$HOME/D/mlink-work/.secrets"
PASS="$SECRETS/sudo.pass"
LOG="$HOME/D/mlink-work/logs/sudo.log"

[ $# -gt 0 ] || { echo "usage: msudo.sh <command> [args...]" >&2; exit 2; }
[ -f "$PASS" ] || { echo "msudo: $PASS missing (user must create it)" >&2; exit 1; }
# Refuse a password file others can read: it is equivalent to root on this box.
[ "$(stat -c %a "$PASS")" = 600 ] || { echo "msudo: $PASS must be mode 600" >&2; exit 1; }

# The askpass helper lives next to the secret, outside the wiped mirror.
ASK="$SECRETS/askpass"
if [ ! -x "$ASK" ]; then
	printf '#!/bin/sh\nexec cat "%s"\n' "$PASS" > "$ASK"
	chmod 700 "$ASK"
fi

mkdir -p "$(dirname "$LOG")"
{ printf '%s' "$(date -Is)"; printf ' %q' "$@"; echo; } >> "$LOG"

rc=0
SUDO_ASKPASS="$ASK" sudo -A -- "$@" || rc=$?
echo "$(date -Is) rc=$rc" >> "$LOG"
exit $rc
