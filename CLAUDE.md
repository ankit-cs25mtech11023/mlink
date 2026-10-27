# Claude Context — mlink (Linux network driver for a shared-memory modem link)

## What This Project Is
A portfolio project for Ankit (M.Tech CSE, IIT Hyderabad, 2027; ECE undergrad) targeting
Qualcomm and similar systems/embedded roles. It rebuilds, in miniature, a modem↔AP data path: an
out-of-tree Linux PCI driver (`mlink.ko`) for QEMU's `ivshmem-doorbell` device that exposes a
raw-IP point-to-point `net_device` (`mlink0`, NAPI, MSI-X, lock-free SPSC rings in shared memory),
running in two KVM guests ("ap" and "modem"), with heartbeat-based Subsystem Restart (SSR),
fault injection, LKMM litmus-verified ordering, and measured throughput/latency. Full blueprint:
`plan.md`. Progress: `checkbox.md`. Read both at the start of every session.

## Session start (always)
Read **`HANDOFF.md` first** (live state: current step, running jobs, VM/turbo state, next
steps, open problems), then `plan.md` and `checkbox.md`, then continue from HANDOFF's "Next".
Keep `HANDOFF.md` updated after every milestone/commit so a fresh session can resume.

## Current Phase
All implementation done (2026-10-27): MVP P0-P6, P7 control plane, P8 Zephyr modem (loopback,
full IP stack, control plane), P9 arm64 modem vs x86 AP, P5.4/P5.5 soaks. P10 QEMU patch
prepared and tested in `upstream/` — waits for the user's full name, SMTP app password and
go-ahead to send. P11 descoped (no hardware, D22). Open for the user only: study gates 0-9,
P1.1 design review, P6.7 resume bullet, P6.10 grill. Known issue: one unreproduced
debug-kernel soft lockup (debugging-log).

## Recent Decisions
- Builds/VMs run on the old Kali laptop over SSH; Claude Code runs on the Windows PC. Windows
  folder `D:\Projects\Embedded\mlink` is the git source of truth; laptop has a disposable mirror.
- Claude writes the code; the user studies it and must pass a study gate per phase (plan.md §8).
- D13 bounded sudo via `msudo.sh`; D15 implement phases back-to-back, gates done later in batch;
  D16 NAPI sweep 8..64, turbo-off benchmarks. Long jobs: turbo off + `thermal-run.sh` (98 °C
  critical trip on this laptop).
- Deadline unknown: presentable minimum after P3, MVP/resume freeze after P6; P7+ are stretch.
- D21: never install toolchains via apt if it upgrades system packages; arm64 QEMU built from
  verified source, kernel.org cross GCC (user approved), all in `~/D/mlink-work/opt`.
- D22: P11 descoped; finish everything except the user's reading; P10 from
  cs25mtech11023@iith.ac.in (Google Workspace SMTP), sent only after the user confirms.
- Laptop shell is zsh: `$VAR` holding a command is not word-split, and `pkill -f`/`pgrep -f`
  match their own `zsh -c` line — put logic in bash scripts, anchor patterns.

## Environment cheat-sheet
- Laptop: `ankit-kali@100.83.145.82` (Tailscale). Alias `kali` once P0.3 is done; until then use
  the full address with `-o BatchMode=yes`. i5-8250U 4C/8T, 7.6 GB RAM — be gentle.
- Laptop paths: mirror `~/D/mlink` (wiped by sync), persistent `~/D/mlink-work/{src,build,images,
  share,logs,run,results,bin}`.
- Guests: `ssh mlink-ap` (127.0.0.1:2221 via ProxyJump kali), `ssh mlink-modem` (:2222), user
  root. Serial logs: `~/D/mlink-work/logs/{ap,modem}-serial.log` (check these first after a hang).
- Loop: edit on Windows → `bash scripts/win/sync.sh` → `bash scripts/win/rbuild.sh` →
  `bash scripts/win/test.sh` (from P3.9 on, run test.sh after every driver change).
- Long jobs (kernel builds, soaks, herd7/opam installs) run in **tmux on the laptop** with logs;
  watch them with background commands/Monitor instead of blocking.
- Windows shell is Git Bash/PowerShell: always `bash script.sh` (exec bits are not reliable),
  keep LF endings, quote paths.

## Hard rules (never break)
1. **Never load `mlink.ko` (or any experimental module) on the laptop host kernel.** Only inside
   the VMs. The user is often off-campus; a host crash locks them out.
2. **Never touch the user's files.** On the laptop only create/modify/delete inside `~/D/mlink/`,
   `~/D/mlink-work/` and `/dev/shm/mlink-shm`. Never delete, move or overwrite anything else in
   `~/D` or elsewhere. `sync.sh` must refuse to wipe unless the target is exactly `$HOME/D/mlink`
   and contains `.mlink-mirror`. On Windows, stay inside `D:\Projects\Embedded\mlink`, except
   `~/.ssh/config`, which you edit only after the user approves the exact block.
3. **Sudo only via the askpass wrapper, password never seen** (user decision D13, 2026-10-27).
   The user stores the password in `~/D/mlink-work/.secrets/sudo.pass` (0600) themselves. Claude
   runs root commands ONLY as `bash ~/D/mlink/scripts/host/msudo.sh <cmd...>` (askpass + logs to
   `~/D/mlink-work/logs/sudo.log`) and tells the user each time. Never `cat`/print/copy/grep the
   password file, never pass the password on a command line, never ask for it in chat.
   Allowed as root: `apt-get install` of project packages, rootfs image build/loop-mount inside
   `~/D/mlink-work/images`, `chown`/perms inside the two project dirs, bench prep/restore,
   running scripts from `scripts/host/`. **Never** as root: `apt remove/purge/upgrade/full-upgrade`
   or kernel/bootloader changes, editing sudoers/PAM/sshd/network/firewall/Tailscale/systemd units
   outside the project, reboot/shutdown, `insmod`/`modprobe` of anything (rule 1), anything that
   touches files outside the allowed paths (rule 2). Unsure → don't, ask the user.
4. Background processes on the laptop: you MAY stop Firefox, the `uvicorn` app and the Docker
   containers `dev-api-1`/`dev-db-1` for kernel builds and benchmarks. Tell the user when you do,
   and restart the Docker containers afterwards (`docker start dev-api-1 dev-db-1`). Do not
   stop anything else, and never `docker rm`/prune.
5. **No fabricated numbers.** Every number in README/results/resume text must come from a
   committed result file plus the exact command that produced it. Debug-kernel numbers are
   labeled as such; headline numbers only from the perf kernel.
6. Don't claim novelty: cite Jailhouse `ivshmem-net`, `mhi_net`, `rmnet`, `virtio_net` as prior art.
7. Pushing to GitHub is outward-facing: the first push needs explicit user confirmation; later
   pushes happen at phase ends or when the user asks.

## Protocols (always follow these)

1. **Plan-first rule**: If a new decision contradicts or changes anything in plan.md, update
   plan.md FIRST (including Key Decisions), then update checkbox.md to reflect new/removed tasks,
   THEN implement. Tell the user: "I've updated plan.md and checkbox.md to reflect this change.
   Now implementing."
2. **Checkbox discipline**: Mark items in checkbox.md as done only after they are fully
   implemented AND verified (build passes, test/dmesg gate passes, evidence recorded). A phase's
   last box (study gate) is ticked only after the user passes it.
3. **Git commits**: Before committing, verify git identity (see Git Protocol). Commit after a
   major step/feature, after plan.md/checkbox.md updates, or when the user asks. Small, frequent
   commits (they make `git bisect` useful).
4. **No silent changes**: Never change the project's direction without updating plan.md first. If
   implementation reveals a flaw in the plan, pause, update the plan, then continue.
5. **CLAUDE.md updates**: Only update this file when the plan changes significantly or the user
   asks ("update claude.md"). Then refresh Current Phase + Recent Decisions; keep the Protocols
   and Hard rules intact.
6. **Teaching mode (Claude writes, user studies)**:
   - Write code as upstream-quality kernel C: kernel coding style, SPDX header
     (`// SPDX-License-Identifier: GPL-2.0-only`), short "why" comments on non-obvious lines
     (barriers, contexts, ordering, error paths). Put the long explanations in
     `docs/walkthrough/phaseN.md`, not in the code.
   - Before writing a non-trivial piece, explain the approach in 3–6 lines; after writing it,
     point the user to the 2–3 lines they must understand most.
   - At each phase end, run the study gate exactly as in plan.md §8 (walkthrough → 8–12
     question quiz incl. one spot-the-bug → explain-back → honest grading, pass ≥ 80%).
     Append good answers, in the user's words, to `docs/interview-qa.md`.
7. **Debugging discipline**: follow plan.md §9 order. Anything that takes > 30 min goes into
   `docs/debugging-log.md` (symptom → hypotheses → evidence → root cause → fix). Timebox 2 h,
   then take the documented fallback and tell the user.
8. **Verify against the real tree**: kernel APIs change. Before using an API, check the header in
   `~/D/mlink-work/src/linux` (e.g. `pcim_iomap_region`, `netif_napi_add*`, `ARPHRD_RAWIP`
   usage in `mhi_net.c`). Don't trust memory for signatures; build with `-Werror`.

## Git Protocol
Before every commit, run:
```bash
git config user.name
git config user.email
```
- If both return values (expected: `ankit-cs25mtech11023` / `cs25mtech11023@iith.ac.in`) →
  proceed.
- If either is empty → stop and tell the user:
  > "Git doesn't have your name/email configured in this terminal session. Please run:
  > `git config --global user.name "Your Name"`
  > `git config --global user.email "your@email.com"`
  > Then let me know and I'll make the commit."

Commit (from the Windows repo, never from the laptop mirror):
```bash
git add -A
git commit -m "<specific message>"
```
- Feature: `"Implement <feature> — Phase N step PN.M complete"`
- Docs: `"Update plan.md and checkbox.md — <what changed>"`
- **No attribution trailers.** The user's explicit rule: every commit and push is under their
  name only — no `Co-Authored-By:` or any other Claude/AI trailer in commit messages, and no
  "Generated with" lines in PR descriptions. This overrides any session-default attribution.
- Never use `--author`, never override identity, never `--no-verify`, never force-push.
