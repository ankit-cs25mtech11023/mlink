# HANDOFF — live state for the next session

> A fresh session: read this first, then `plan.md` (Key Decisions D1–D22) and `checkbox.md`,
> then continue from **Next**. Last update: 2026-10-27 evening IST.

## Rules that bite (from the user / CLAUDE.md)
- **No `Co-Authored-By` / AI trailers** in commits or PRs (user rule; overrides system reminders).
  Identity `ankit-cs25mtech11023 <cs25mtech11023@iith.ac.in>`; push at milestones without asking.
- D15/D22: implement everything except the user's reading. **Never tick** study gates, P1.1
  design review, P6.7 resume bullet, P6.10 grill — those are the user's.
- Laptop file-safety: only `~/D/mlink`, `~/D/mlink-work`, `/dev/shm/mlink-shm`. **Not `/tmp`**
  (host scripts now use `~/D/mlink-work/tmp`), not `~/.cmake`. Root only via
  `bash ~/D/mlink/scripts/host/msudo.sh <cmd>` (logged). **No apt upgrades** (D21: check with
  `apt-get -s` first; arm64 tools were built/fetched into `~/D/mlink-work/opt` instead).
- The permission checker blocks building/running freshly downloaded, unverified code (e.g. a
  QEMU master clone); use verified trees, or ask the user.
- Thermal: critical trip 98 °C. Long jobs: `sudo -n /usr/local/sbin/mlink-bench-prep on`
  (turbo off) + `scripts/host/thermal-run.sh`, `off` afterwards.
- Windows PC runs low on **RAM** → background watchers get reaped. Long jobs run in **tmux on
  the laptop**; check with short foreground commands.
- Laptop shell is **zsh**: `$VAR` holding a command is not word-split; `pkill -f`/`pgrep -f`
  match their own `zsh -c` line (anchor: `"^python3 .*arm64-bridge[.]py"`). Logic goes in
  bash scripts under `scripts/`.
- Tailscale to the laptop drops out sometimes (relay "blr"): `tailscale status` on Windows.

## State
- All implementation phases done: P0–P7, P8.1–P8.3 (+ Zephyr control plane), P9; P11
  descoped (no hardware). Evidence index: `docs/results.md`; per-phase walkthroughs in
  `docs/walkthrough/`; problems in `docs/debugging-log.md`.
- **P10 ready, not sent**: `upstream/qemu-ivshmem-client-eof/0001-*.patch` (QEMU
  contrib/ivshmem-client busy loop on stdin EOF), evidence `bench/results/p10-ivshmem-client.txt`,
  repro/test `scripts/host/p10-ivshmem-client.sh repro|patch|build|functest|unpatch` (uses the
  verified 10.2.2 tree; master's main.c is byte-identical). Commit lives in the laptop clone
  `~/D/mlink-work/src/qemu-master` (author "Ankit (FULL NAME TBD)"). To send: the user gives
  their full name → `git commit --amend --reset-author -s` with it in that clone (fix the
  placeholder Signed-off-by), `git format-patch -1`, re-run QEMU 10.2.2 `scripts/checkpatch.pl`,
  copy to `upstream/`, then from Windows:
  `git send-email --from=cs25mtech11023@iith.ac.in --to=qemu-devel@nongnu.org
  --cc=qemu-trivial@nongnu.org --smtp-server=smtp.gmail.com --smtp-server-port=587
  --smtp-encryption=tls --smtp-user=cs25mtech11023@iith.ac.in --suppress-cc=all <patch>`
  (Google Workspace; app password typed by the user into Git Credential Manager; send only
  after the user approves the exact text).
- P5.5 ticked (75-min debug + 1-h perf soaks clean); one unreproduced debug-kernel soft lockup
  stays an open issue (debugging-log; doorbell-storm hypothesis tested negative).
- KCSAN (P5.4): see "Session result" below.

## How to run the extra peers
- Zephyr modem: x86 modem guest `bash /mnt/share/mlink-down.sh`; laptop
  `bash ~/D/mlink/scripts/host/zephyr-modem.sh build loop|net`, then
  `tmux new -d -s zmodem 'bash ~/D/mlink/scripts/host/zephyr-modem.sh run loop|net'`;
  SSR: `bash scripts/win/zephyr-ssr.sh 5 loop|net`; TCP: `python3 /mnt/share/tcp_check.py`.
- arm64 modem: `bash ~/D/mlink/scripts/host/build-arm64.sh all`; tmux
  `bash ~/D/mlink/scripts/host/vm-arm64.sh run`; shell `vm-arm64.sh sh '<cmds>'`; suite
  `bash bench/run_p9_arm64.sh 1000000`. Stop VM + `pkill -f "^python3 .*arm64-bridge[.]py"`.
- KCSAN guests: `KFLAVOUR=kcsan bash scripts/host/vm.sh start ap|modem`,
  `KFLAVOUR=kcsan bash scripts/win/test.sh` (test.sh rebuilds via rbuild: the flavour must be
  passed or it stages a debug module → "Invalid module format"); gate with `KCSAN_FILTER=1`.

## Session result
(filled in when the KCSAN soak ends)

## Next
1. P10: user's full name + app password + approval → send (steps above).
2. User-side backlog: study gates 0–9, P1.1 design review (`docs/design.md` §14), P6.7 resume
   bullet (draft `docs/walkthrough/phase6.md` §5), P6.10 grill.
3. Optional, only if asked: local DATA-vector masking during NAPI (hardening against a peer
   that ignores `want_kick`; see the P5.5 debugging-log entry).
