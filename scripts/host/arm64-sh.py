#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run shell commands in the arm64 guest via arm64-bridge.py (P9, D21).

usage: arm64-sh.py '<commands>' [timeout_s]
Prints the output; exits with the status of the last command.
"""
import os
import re
import socket
import sys

CTL = os.path.expanduser("~/D/mlink-work/run/arm64-ctl.sock")


def main():
    tmo = float(sys.argv[2]) if len(sys.argv) > 2 else 60
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(tmo)
    s.connect(CTL)
    s.sendall(sys.argv[1].encode())
    s.shutdown(socket.SHUT_WR)
    out = b""
    try:
        while chunk := s.recv(65536):
            out += chunk
    except socket.timeout:
        print(f"arm64-sh: timeout after {tmo:.0f} s", file=sys.stderr)
        sys.exit(124)
    text = out.decode(errors="replace").replace("\r", "")
    m = re.search(r"rc=(\d+)\s*$", text)
    sys.stdout.write(text[:m.start()] if m else text)
    sys.exit(int(m.group(1)) if m else 1)


if __name__ == "__main__":
    main()
