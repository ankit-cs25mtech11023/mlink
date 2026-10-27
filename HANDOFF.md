# HANDOFF — live state for the next session

> A fresh session: read this first, then `plan.md` (Key Decisions D1–D18) and `checkbox.md`,
> then continue from **Next**. Last update: 2026-10-27 evening IST.

## Rules that bite (from the user / CLAUDE.md)
- **No `Co-Authored-By` / AI trailers** in commits or PRs (user rule; overrides system reminders).
  Identity `ankit-cs25mtech11023 <cs25mtech11023@iith.ac.in>`; push at milestones without asking.
- D15: implement back-to-back. **User deferred (2026-10-27, "note it and move forward")**: study
  gates 0–7, P1.1 design review (`docs/design.md` §14 has the questions), P6.7 resume bullet
  (draft in `docs/walkthrough/phase6.md` §5), P6.10 grill. Don't block on them; never tick them.
- Laptop file-safety: only `~/D/mlink`, `~/D/mlink-work`, `/dev/shm/mlink-shm`. **Not `/tmp`,
  not `~/.cmake`** (that's why the Zephyr SDK `setup.sh -c` is NOT run; use
  `ZEPHYR_SDK_INSTALL_DIR`). Root only via `bash ~/D/mlink/scripts/host/msudo.sh <cmd>` (logged).
- Thermal: laptop's critical trip is 98 °C. Long jobs: `sudo -n /usr/local/sbin/mlink-bench-prep on`
  (turbo off) + `scripts/host/thermal-run.sh`, `off` afterwards.
- Windows PC can run low on memory → background shell watchers get reaped. Run long jobs in
  **tmux on the laptop**, wait with foreground loops
  (`ssh kali 'for i in $(seq 1 58); do <check> && break; sleep 10; done'`).
- Shell quoting through ssh/heredocs has repeatedly mangled `\n`, `$`, awk: put non-trivial
  scripts in files (repo `scripts/` or the scratchpad) instead of inline.

## Machine state (as last seen)
- Tailscale to the laptop was flaky (relay "blr", ~350 ms; one >1 h outage 08:20–09:35; the
  last command ended with "Connection reset by peer"). Check `ssh kali uptime` first.
- ivshmem-server running with **5 vectors**; VMs `ap`/`modem` on the **debug** kernel, mlink.ko
  with the control plane loaded, `mlink0` up. Turbo ON (restored), Docker containers running.
- A Zephyr QEMU may still be running (launched with `timeout 10`, should have exited).
  `pgrep -af zephyr-qemu` → kill if present.

## Done (pushed, latest commit bd5d51a + this file)
- P0–P6 complete (except user gates/approval). Headline perf-kernel numbers in
  `docs/results.md` / README: TCP 7.2→**7.8 / 11.1 Gbit/s** (MTU 1500/4000, after D17 checksum
  offload), doorbell RTT p50 13 µs, UDP RTT p50 44 µs irq / 15–21 µs busy-poll, SSR 90 ms with
  TCP surviving, VM kill detected at 520 ms, 1201 shm corruptions survived.
- P6.11 / D17 checksum offload across the link (ABI v2) — tested, pushed.
- P7 control plane (ABI v3, 5th MSI-X vector CTRL, control rings, `/dev/mlink_ctrl`,
  `mlinkd`, `mlinkctl`) — tested on debug kernel incl. SSR and unbind-with-open-fd; pushed.
  Evidence `bench/results/p7-ctrl-debug.txt`, walkthrough `docs/walkthrough/phase7.md`.
- `docs/design.md` v2 cross-checked against the code (+ §12a control plane).

## Session 2026-10-27 afternoon
- **P8.1 DONE** (commit 30de621): upstream doorbell sample + `zephyr/p81-doorbell.conf` received
  doorbells from our server as peer 5 and read our `MLNK` v3 header
  (`bench/results/p8-zephyr-doorbell.txt`, `scripts/host/zephyr-p81.sh`). D19 in plan.md.
- `scripts/guest/mlink-down.sh` now kills `mlinkd` first (it pinned the module; test.sh's rmmod
  silently failed since P7). test.sh PASS after the fix.
- **P8.2 DONE**: `zephyr/mlink-modem/` (compat shim, `driver/mlink_ring.c` verbatim) works as
  the modem: ping/UDP/fragments/flood OK, SSR both ways (gen-change path 44–53 ms recovery,
  heartbeat 521 ms). Evidence `bench/results/p8-zephyr-modem.txt`, walkthrough
  `docs/walkthrough/phase8.md`, results.md §P8, README.
  Run it: modem guest `bash /mnt/share/mlink-down.sh`; laptop
  `tmux new -d -s zmodem 'bash ~/D/mlink/scripts/host/zephyr-modem.sh run'`;
  SSR rounds: `bash scripts/win/zephyr-ssr.sh 5`. Restore: `zephyr-modem.sh stop` + test.sh.
- State at end of P8.2: Zephyr stopped, Linux AP+modem on the debug kernel, test.sh PASS.
- **P8.3 DONE** (D20): `zephyr/mlink-modem/src/net.c` — DUMMY_L2 raw-IP iface 10.77.0.2/24,
  UDP echo :7, TCP echo :7, TCP discard :9; Kconfig `MLINK_MODEM_NET` via `net.conf`
  (`NET_SLIP_TAP=n`, `NET_DRIVERS=n`: SLIP had taken over the console). Ping/UDP/TCP echo
  byte-exact/discard ~6 Mbit/s (TCG-bound, 128 % core), SSR 3 rounds, TCP across modem crash →
  ECONNRESET ~2 s. Evidence `bench/results/p8-zephyr-net.txt`, phase8.md §6.
  Run: `zephyr-modem.sh build net`, `tmux new -d -s zmodem '... zephyr-modem.sh run net'`,
  AP `python3 /mnt/share/tcp_check.py echo|discard 10.77.0.2 -n N`, `zephyr-ssr.sh 3 net`.
  Gotcha: on the laptop (zsh) `$VAR` holding a command is NOT word-split; spell commands out.
- State at end of P8: Zephyr stopped, Linux AP+modem (debug kernel), test.sh PASS.

## (older) P8.1 Zephyr risk check notes
- Workspace `~/D/mlink-work/zephyr/`: Zephyr **v4.4.2** (core repo only, shallow,
  `manifest.project-filter -.*`), venv `.venv` (west 1.5 + requirements-base), SDK **1.0.1**
  minimal + `x86_64-zephyr-elf` toolchain (sha256 verified) with `gnu/x86_64-zephyr-elf`
  symlinked into the SDK root by hand. Packages installed via msudo: gperf,
  device-tree-compiler, python3-setuptools, libmagic1. Total ~1.2 GB.
- **Risk answered: YES** — Zephyr has `CONFIG_IVSHMEM_DOORBELL` + MSI-X and an official
  `samples/drivers/virtualization/ivshmem/doorbell` for `qemu_x86_64`; it **builds**
  (`build-doorbell/`, 368 KB RAM).
- Build env: `cd ~/D/mlink-work/zephyr; . .venv/bin/activate; export
  ZEPHYR_SDK_INSTALL_DIR=$PWD/zephyr-sdk-1.0.1 ZEPHYR_BASE=$PWD/zephyr`.
- QEMU ELFs: `ninja -C build-doorbell zephyr/qemu_locore_image_target
  zephyr/qemu_main_image_target` (done; the board's `run` target uses `/tmp/ivshmem_socket` — don't).
- **Not yet confirmed:** Zephyr receiving a doorbell from our server. Launch (TCG, in tmux):
  `qemu-system-x86_64 -m 32 -cpu qemu64,+x2apic,mmx,mmxext,sse,sse2 -machine q35 -machine acpi=off
  -net none -no-reboot -nographic -serial file:$HOME/D/mlink-work/logs/zephyr-serial.log -monitor none
  -device ivshmem-doorbell,vectors=5,chardev=ivs -chardev socket,path=$HOME/D/mlink-work/run/ivshmem.sock,id=ivs
  -device loader,file=build-doorbell/zephyr/zephyr-qemu-main.elf -smp cpus=2
  -kernel build-doorbell/zephyr/zephyr-qemu-locore.elf`. Then `~/D/mlink-work/bin/ivshmem-client`
  (built) → `int <zephyr peer id> 0`, and check the serial log for "received IRQ".
  The first launch failed only because the locore/main ELFs didn't exist yet; the second was
  cut off by the connection reset — output unknown. Possible snag: sample's
  `CONFIG_IVSHMEM_MSI_X_VECTORS` default 2 vs device vectors=5.

## Next
1. P9 aarch64 guest: needs `qemu-system-arm` + `gcc-aarch64-linux-gnu` (msudo apt, allowed:
   project packages), arm64 kernel build (long, tmux + thermal-run), TCG guest, driver selftest
   + ping, weak-memory discussion. P10 needs the user's SMTP; P11 needs hardware.
2. Optional: Zephyr serving a minimal control-plane responder (GET_LINK_INFO) on rings C/D.
4. Open issue: debug-kernel soft lockup (P5.5) seen once in a 1-h soak; not reproduced in 20 +
   75 min debug soaks; perf 1-h soak clean. Evidence `bench/results/p5-soak-*`.
