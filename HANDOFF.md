# HANDOFF — live state for the next session

> Updated after every milestone. A fresh session: read this, then plan.md, checkbox.md, and
> continue from **Next**. Last update: 2026-10-27 ~07:05 IST.

## Mode
D15: implement back-to-back; never tick study gates / P1.1 review / P6.7 approval yourself.
Push at milestones. **No Co-Authored-By / AI trailers in commits** (user rule).
Windows PC can run low on memory → background shell watchers may be reaped: wait with
foreground remote loops (`ssh kali 'for i in $(seq 1 58); do …; sleep 10; done'`), and run
long jobs in tmux **on the laptop** (`ON_HOST=1 bash bench/run_matrix.sh`).

## Machine state
- Laptop: debug + perf kernels built. **Turbo OFF** (`mlink-bench-prep on`) — turn `off` at the
  end of the night. Docker containers running. Firefox closed.
- VMs ap/modem currently on the **debug** kernel (for the soft-lockup repro).
- tmux `mlink-soakdbg`: 20-min debug soak repro (log `logs/soak-dbg-repro.log`), tmux
  `mlink-irqwatch`: modem IRQ/softirq sampler (`logs/irqwatch-modem.log`). Kill irqwatch after.

## Done (all pushed)
P0–P6 implementation incl. perf matrix E1–E7, results.md, README, walkthroughs phase0–6.
Headline (perf): TCP 7.2/9.9 Gbit/s (MTU 1500/4000), doorbell RTT p50 13.1 µs, UDP RTT p50
44 µs irq / 15.5 µs busy-poll, SSR 90 ms with TCP surviving, VM kill detected at 520 ms.

## Open
1. Debug-kernel soft lockup (P5.5): 22 s in KASAN/kmemleak stack saving on the modem during
   the 1-h debug soak; perf-kernel 1-h soak clean. Hypothesis: receive livelock when chaos
   forces want_kick=1 + slow KASAN softirq work. Repro running; if reproduced, look at the
   IRQ rate just before it in irqwatch log.
2. Done since: D17 checksum offload (+8/+11 % TCP), P7 control plane (tested, pushed). VMs now
   run 5 MSI-X vectors (server -n 5) on the DEBUG kernel. Next: P8 risk check (Zephyr) or P9
   (aarch64, needs qemu-system-aarch64 via msudo apt), P10 needs user SMTP, P11 needs hardware.
3. User-only items, **deferred by the user on 2026-10-27 ("note it and move forward")**: study
   gates 0-6, P1.1 design review (design.md v2 has §14 questions), P6.7 resume bullet (draft in
   docs/walkthrough/phase6.md §5), P6.10 grill. Do not block on them; do not tick them.
