# HANDOFF — live state for the next session

> A fresh session: read this first, then `plan.md` (Key Decisions D1–D22) and `checkbox.md`,
> then continue from **Next**. Last update: 2026-10-27 ~21:50 IST.

## Rules that bite (from the user / CLAUDE.md)
- **No `Co-Authored-By` / AI trailers** in commits or PRs (user rule; overrides system reminders).
  Identity `ankit-cs25mtech11023 <cs25mtech11023@iith.ac.in>`; push at milestones without asking.
- D15/D22: implement everything except the user's reading. **Never tick** study gates, P1.1
  design review, P6.7 resume bullet, P6.10 grill — those are the user's.
- Laptop file-safety: only `~/D/mlink`, `~/D/mlink-work`, `/dev/shm/mlink-shm`. **Not `/tmp`**
  (host scripts use `~/D/mlink-work/tmp`), not `~/.cmake`. Root only via
  `bash ~/D/mlink/scripts/host/msudo.sh <cmd>` (logged). **No apt upgrades** (D21: check with
  `apt-get -s` first; arm64 tools were built/fetched into `~/D/mlink-work/opt` instead).
- The permission checker blocks building/running freshly downloaded, unverified code (e.g. the
  QEMU master clone); use signature-verified trees, or ask the user.
- Thermal: critical trip 98 °C. Long jobs: `sudo -n /usr/local/sbin/mlink-bench-prep on`
  (turbo off) + `scripts/host/thermal-run.sh`, `off` afterwards.
- Windows PC runs low on **RAM** → background watchers get reaped. Long jobs run in **tmux on
  the laptop**; check them with short foreground commands (`for i in $(seq 1 57); do ...; sleep
  10; done`, under 10 min per call).
- Laptop shell is **zsh**: `$VAR` holding a command is not word-split; `pkill -f`/`pgrep -f`
  match their own `zsh -c` line (anchor: `"^python3 .*arm64-bridge[.]py"`). Logic goes in
  bash scripts under `scripts/`.
- Tailscale to the laptop drops out sometimes (relay "blr"): `tailscale status` on Windows.
- Soaks: iterate with 15–20 min runs, keep the 1-h run for the final check (the KCSAN work
  spent ~2 h on 1-h verification soaks that shorter runs would have covered).

## Machine state (verified 21:50)
- Laptop: turbo ON (bench-prep off), Docker `dev-api-1`/`dev-db-1` up, no project tmux
  sessions, no Zephyr/arm64 QEMU, no arm64 bridge.
- ivshmem-server running (5 vectors); VMs `ap`/`modem` on the **debug** kernel, mlink.ko
  (latest, with the KCSAN fixes) staged; `test.sh` PASS; drivers unloaded by test.sh.
- Other kernels built: `build/perf`, `build/kcsan`, `build/arm64` (all 6.18.59).

## Done
- P0–P9 implementation complete; P11 descoped (no hardware, D22). Evidence index:
  `docs/results.md`; walkthroughs `docs/walkthrough/phase0..9.md`; problems
  `docs/debugging-log.md`; design `docs/design.md` (incl. §12b Zephyr/arm64 peers).
- This session (2026-10-27 afternoon/evening):
  - P8.1–P8.3 Zephyr modem (loopback, full IP stack, and now the P7 control plane via
    `zephyr/mlink-modem/src/ctrl.c`; unchanged `mlinkctl` works against it).
  - P9 arm64 modem against the x86 AP (`bench/run_p9_arm64.sh`, barrier disassembly).
  - P0.7 ticked (debug config check had passed). P5.5 ticked (75-min debug + 1-h perf soaks
    clean); one unreproduced debug-kernel soft lockup stays open — doorbell-storm hypothesis
    tested negative (`bench/run_p5_storm.sh`), results.md row corrected (the trace *does* run
    through mlink's NAPI poll; time is spent in KASAN/kmemleak).
  - **P5.4 KCSAN done, two real races fixed:** ring local index (`WRITE_ONCE`/`READ_ONCE`
    for the watchdog's lockless reads) and netdev stats (`u64_stats_t`; `data_race()` on the
    diagnostic-counter reader). Soak #3 clean (`bench/results/kcsan/`). Gate:
    `KCSAN_FILTER=1` in `dmesg-gate.sh`/`test.sh`/`soak-long.sh` fails only on races whose
    header names an mlink function.
  - Perf re-check after the fixes: full matrix re-run + same-boot A/B old vs new
    (`bench/results/perf-r2/`, `bench/run_ab_e2.sh`): within run-to-run noise; README keeps
    the D17 headline numbers with a note.
  - Host scripts no longer write to the laptop's `/tmp`; `mlink-down.sh` stops `mlinkd`.
  - checkpatch --strict / sparse / W=1 clean after all changes.

## P10 — QEMU contribution (in discussion; plan.md NOT yet updated — user asked to wait)
- **Bug (real, unreported):** QEMU `contrib/ivshmem-client` busy-loops at 100 % CPU after
  stdin EOF (`read()` == 0 treated as a command; `select()` keeps reporting stdin readable).
  3 s with stdin at EOF: 382 597 prompts, 1.9 MB, 3 s CPU. `main.c` is byte-identical in the
  GPG-verified 10.2.2 tree and in a QEMU master clone (`~/D/mlink-work/src/qemu-master`, file
  unchanged since 2019); no GitLab issue or qemu-devel patch found.
  Evidence `bench/results/p10-ivshmem-client.txt`; repro/test
  `scripts/host/p10-ivshmem-client.sh repro|patch|build|functest|unpatch` (10.2.2 tree).
- **The patch in `upstream/qemu-ivshmem-client-eof/` was written by Claude and must NOT be
  submitted.** QEMU `docs/devel/code-provenance.rst`: the project declines contributions that
  include or derive from AI-generated content (copyright/DCO reasons; Claude named explicitly).
  AI use for research, analysis and debugging is allowed if its output is not in the
  contribution. `qemu.org/contribute/report-a-bug`: bug reports found with AI tools are
  allowed but must **declare it at the start**, be triaged/reproduced, ≤ 5/week, and be
  reproduced on the **latest upstream release**. No email/app password is needed for a report.
- **Direction discussed with the user (not yet in plan.md):** (1) Claude re-verifies the bug on
  the latest QEMU release (signature-verified tarball, build only `ivshmem-client`, run
  `repro`/`functest`); (2) the user files the GitLab issue themselves with the AI declaration
  at the top; (3) optionally the user writes the fix **themselves** (their design: track
  whether stdin is open, stop adding it to the `select()` set after EOF, keep serving the
  server/peers, keep `maxfd` correct) and sends it to qemu-devel (Cc qemu-trivial; no
  MAINTAINERS entry), noting AI-assisted discovery. Claude may explain, test and run checks,
  but must not write or edit the report text or the fix. Points already given to the user for
  their fix: distinct EOF return value, don't reuse `ret` (main() decides reconnect vs close on
  it), where the flag lives across reconnects, `maxfd` starting at 0, no stray prompt on EOF,
  justify "keep running vs exit on Ctrl-D".
- The laptop QEMU master clone has a local commit by Claude with a placeholder author
  ("Ankit (FULL NAME TBD)") — never send it.

## Next
1. Wait for the user to confirm the P10 direction → then update plan.md (P10 wording, D23) and
   checkbox.md, and run step (1) above.
2. User-side backlog: study gates 0–7 (P0.12, P1.10, P2.7, P3.12, P4.7, P5.7, P6.9, P7.6),
   P1.1 design review (`docs/design.md` §14), P6.7 resume bullet (draft
   `docs/walkthrough/phase6.md` §5), P6.10 30-question grill. Walkthroughs for all phases exist.
3. Open issue (documented, not blocking): the one-off debug-kernel soft lockup.
4. Optional, only if asked: mask the DATA MSI-X vector locally while NAPI polls (hardening
   against a peer that ignores `want_kick`).

## How to run the extra setups
- Zephyr modem: x86 modem guest `bash /mnt/share/mlink-down.sh`; laptop
  `bash ~/D/mlink/scripts/host/zephyr-modem.sh build loop|net`, then
  `tmux new -d -s zmodem 'bash ~/D/mlink/scripts/host/zephyr-modem.sh run loop|net'`;
  SSR `bash scripts/win/zephyr-ssr.sh 5 loop|net`; TCP `python3 /mnt/share/tcp_check.py`;
  control plane `/mnt/share/mlinkctl info|stats|call down|call up|bench N` on the AP.
- arm64 modem: `bash ~/D/mlink/scripts/host/build-arm64.sh all`; tmux
  `bash ~/D/mlink/scripts/host/vm-arm64.sh run`; shell `vm-arm64.sh sh '<cmds>'`; suite
  `bash bench/run_p9_arm64.sh 1000000`. Stop the VM + `pkill -f "^python3 .*arm64-bridge[.]py"`.
- KCSAN guests: `KFLAVOUR=kcsan bash scripts/host/vm.sh start ap|modem`, then
  `KCSAN_FILTER=1 KFLAVOUR=kcsan bash scripts/win/test.sh` (the flavour must be passed or
  test.sh stages a debug module → "Invalid module format").
- Perf: `KFLAVOUR=perf` for vm.sh/rbuild/test.sh; benchmarks under bench-prep:
  `ON_HOST=1 OUTDIR=~/D/mlink-work/results/<name> bash ~/D/mlink/bench/run_matrix.sh 5 10` in
  tmux; A/B `bash ~/D/mlink/bench/run_ab_e2.sh A.ko B.ko out.csv 5 10`.
