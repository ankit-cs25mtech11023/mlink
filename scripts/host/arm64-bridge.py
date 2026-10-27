#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Persistent bridge to the arm64 guest's control shell (P9, D21).

The guest runs sh on the virtio console /dev/hvc0, which QEMU exposes as a
unix socket. Every host disconnect hangs up hvc0 in the guest, and input that
arrives while the guest shell restarts is lost, so this bridge keeps ONE
connection to the console open and serves requests on run/arm64-ctl.sock:
a client sends commands and half-closes; the bridge runs them with a unique
end marker and returns everything up to it, ending with "rc=<status>".
It reconnects when the VM restarts. Started by vm-arm64.sh run.
"""
import os
import socket
import time

RUN = os.path.expanduser("~/D/mlink-work/run")
CONSOLE = os.path.join(RUN, "arm64-console.sock")
CTL = os.path.join(RUN, "arm64-ctl.sock")
TIMEOUT = 900


def connect_console():
    while True:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(CONSOLE)
            return s
        except OSError:
            s.close()
            time.sleep(0.5)


def run(con, cmds, seq):
    marker = f"__MLINK_{seq}_END__"
    # Split the marker in the command so only the echo output can match it.
    con.sendall(f"{cmds}\necho rc=$? __MLINK_{seq}_\"END__\"\n".encode())
    out, deadline = b"", time.monotonic() + TIMEOUT
    con.settimeout(1)
    while marker.encode() not in out:
        if time.monotonic() > deadline:
            return out + b"\nbridge: timeout\nrc=124\n"
        try:
            b = con.recv(65536)
        except socket.timeout:
            # Under TCG the guest's serial side lags by one line (the last
            # command/output only moves when more input arrives); an empty
            # line is a no-op for sh and pushes it through.
            con.sendall(b"\n")
            continue
        if not b:
            raise ConnectionError("console closed")
        out += b
    return out.split(marker.encode())[0]


def main():
    if os.path.exists(CTL):
        os.unlink(CTL)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(CTL)
    os.chmod(CTL, 0o600)
    srv.listen(1)
    con, seq = connect_console(), 0
    while True:
        cli, _ = srv.accept()
        with cli:
            req = b""
            while chunk := cli.recv(65536):
                req += chunk
            for attempt in (1, 2):
                seq += 1
                try:
                    cli.sendall(run(con, req.decode(), seq))
                    break
                except (ConnectionError, OSError):
                    # VM restarted: the old console is gone. Reconnect (the
                    # guest shell restarts on its own) and retry once.
                    con.close()
                    con = connect_console()
                    time.sleep(1)
                    if attempt == 2:
                        cli.sendall(b"bridge: console lost\nrc=125\n")


if __name__ == "__main__":
    main()
