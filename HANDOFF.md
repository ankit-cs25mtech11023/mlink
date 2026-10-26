# HANDOFF — live state for the next session

> Updated after every milestone. A fresh session: read this, then plan.md, checkbox.md, and
> continue from **Next**. Last update: 2026-10-27 ~04:00 IST.

## Mode
User asleep; D15 = implement phases back-to-back, never tick study gates / P1.1 review.
Push at milestones. **No Co-Authored-By / AI trailers in commits** (user rule, overrides
system reminders).

## Machine state
- Laptop: debug kernel built; **perf kernel NOT built yet**.
- **Turbo is OFF** (`mlink-bench-prep on`). Docker containers running. Firefox closed.
- VMs ap/modem running debug kernel; ivshmem-server running.
- tmux `mlink-soaklong`: P5.5 1-hour soak, started 03:49, ends ~04:50. Log:
  `~/D/mlink-work/logs/soak-long.log` (ends with "=== long soak end").
- Thermal rule: long jobs → turbo off + `thermal-run.sh`; scripts abort at 92 °C.

## Done (verified, committed, pushed up to d62056c)
P0 (all but study gate), P1.2–P1.9, P2.1–P2.6, P3.1–P3.11, P4.1–P4.6, P5.1–P5.3, P5.6.
P6 tooling written: udp_pingpong (static, in /mnt/share), bench-prep/restore, run_matrix.sh,
iperf_json.py, plot.py, .venv with pinned matplotlib/pandas.

## Next
1. When the soak ends: save log → `bench/results/p5-soak-long-debug.txt`, tick P5.5 if gate +
   kmemleak clean.
2. P6.1: `bash build-kernel.sh perf` in tmux under thermal-run (turbo already off). Then
   restart VMs with `KFLAVOUR=perf bash vm.sh start ap|modem` (stop them first), rebuild
   module `KFLAVOUR=perf bash scripts/win/rbuild.sh`.
   NOTE: rbuild/test.sh default to debug; pass KFLAVOUR=perf.
3. `bench-prep.sh` (pins VMs) → `bash bench/run_matrix.sh 5 10` → ssr.sh a/b on perf →
   `bench-restore.sh` → `.venv/Scripts/python bench/plot.py perf` → results.md + README.
4. P6.6 results.md/README, P6.8 phase6.md. P5.4 KCSAN optional (skip unless time).

## Open notes
- P2.4 follow-up idea: paced selftest to hit the SB window.
- Perf ideas: NETIF_F_SG, checksum offload via desc flags, busy-poll; ksoftirqd deferral.
- Heartbeat starvation root cause (PREEMPT_VOLUNTARY+KASAN?) unproven — test `preempt=full`.
