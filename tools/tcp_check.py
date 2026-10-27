#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""TCP checks against the Zephyr modem (P8.3).

  tcp_check.py echo HOST [-n BYTES]     send random bytes to :7, verify the echo
  tcp_check.py discard HOST [-n BYTES]  send to :9, report goodput

Standard library only (runs in the minimal Debian guests).
"""
import argparse
import os
import socket
import sys
import threading
import time


def echo(host, nbytes, chunk=16384):
    data = os.urandom(nbytes)
    got = bytearray()
    s = socket.create_connection((host, 7), timeout=30)

    def reader():
        while len(got) < nbytes:
            b = s.recv(65536)
            if not b:
                break
            got.extend(b)

    t = threading.Thread(target=reader)
    t0 = time.monotonic()
    t.start()
    for off in range(0, nbytes, chunk):
        s.sendall(data[off:off + chunk])
    t.join()
    dt = time.monotonic() - t0
    s.close()
    ok = bytes(got) == data
    print(f"tcp echo: sent {nbytes} B, got {len(got)} B back, "
          f"{'MATCH' if ok else 'MISMATCH'}, {dt:.2f} s, "
          f"{2 * nbytes * 8 / dt / 1e6:.1f} Mbit/s both ways")
    return ok


def discard(host, nbytes, chunk=65536):
    buf = os.urandom(chunk)
    s = socket.create_connection((host, 9), timeout=30)
    t0 = time.monotonic()
    sent = 0
    while sent < nbytes:
        s.sendall(buf[:min(chunk, nbytes - sent)])
        sent += min(chunk, nbytes - sent)
    s.shutdown(socket.SHUT_WR)
    s.recv(1)  # wait for the server to close: everything was received
    dt = time.monotonic() - t0
    s.close()
    print(f"tcp discard: {nbytes} B in {dt:.2f} s = {nbytes * 8 / dt / 1e6:.1f} Mbit/s")
    return True


def main():
    p = argparse.ArgumentParser()
    p.add_argument("mode", choices=["echo", "discard"])
    p.add_argument("host")
    p.add_argument("-n", type=int, default=1 << 20, help="bytes (default 1 MiB)")
    a = p.parse_args()
    ok = echo(a.host, a.n) if a.mode == "echo" else discard(a.host, a.n)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
