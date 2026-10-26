# HANDOFF — live state for the next session

> Updated after every milestone. A fresh session: read this, then plan.md, checkbox.md, and
> continue from **Next**. Last update: 2026-10-27 ~02:20 IST.

## Mode
User asleep; D15 = implement phases back-to-back, never tick study gates / P1.1 review.
Push at milestones. **No Co-Authored-By / AI trailers in commits** (user rule, overrides
system reminders).

## Machine state
- Laptop: debug kernel `~/D/mlink-work/build/debug` (6.18.59-mlink-debug).
- **Turbo is OFF** (`mlink-bench-prep on` since P2 selftest, for thermals). Turn it `off`
  when the night's work ends. Docker containers running; Firefox closed (user's to reopen).
- ivshmem-server + VMs ap/modem running. tmux `mlink-soak` = P3.8 soak (10 min, log
  `~/D/mlink-work/logs/soak.log`).
- Thermal rule: long jobs → turbo off + `thermal-run.sh` (or soak's own 92 °C abort).

## Done (verified, committed, pushed up to f459fd9 + later commits)
P0.1–P0.10, P1.2–P1.8, P2.1–P2.5, P3.1–P3.7, P3.6, P3.9. Walkthroughs phase0/1/2 written.
Key results (debug kernel): doorbell RTT p50 8–12 µs; selftest 10 M msgs 0 errors, 269k
msg/s, suppression 2.9×; litmus 3 Never / 3 Sometimes; TCP ~47 Mbit/s (sender CPU-bound
under KASAN); ping ~1 ms (stack cost + ksoftirqd deferral seen in trace).

## In progress
- P3.8 soak running → check log; if clean tick P3.8, record in bench/results/p3-soak-debug.txt.

## Next
1. P3.10 (numbers already in p3-functional-debug.txt → tick after soak), P3.11 phase3.md.
2. Phase 4: heartbeat + gen handshake + reset/re-handshake + STATE vector + ssr.sh scenarios.
3. P0.11 environment.md. Phase 5 (shm_chaos, fault knobs, checkpatch/sparse), Phase 6 (perf
   kernel, udp_pingpong, matrix).

## Open notes
- P2.4 follow-up idea: paced selftest to hit the SB window.
- Perf ideas for P6: NETIF_F_SG, checksum offload via desc flags (virtio-style), MTU sweep,
  busy-poll; ksoftirqd deferral analysis.
