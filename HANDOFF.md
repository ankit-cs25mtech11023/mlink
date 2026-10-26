# HANDOFF — live state for the next session

> Updated after every milestone. A fresh session: read this, then plan.md, checkbox.md, and
> continue from **Next**. Last update: 2026-10-27 ~01:45 IST.

## Mode
User is asleep; D15 = implement phases back-to-back, don't stop for reviews/study gates
(never tick those yourself). Push to GitHub at milestones (push-policy memory).

## Machine state
- Laptop: debug kernel built (`~/D/mlink-work/build/debug`, 6.18.59-mlink-debug).
  Turbo restored (`mlink-bench-prep off`), Docker containers restarted. Firefox still closed.
- **Thermal rule:** any long job = `sudo -n /usr/local/sbin/mlink-bench-prep on` (turbo off)
  + `bash ~/D/mlink/scripts/host/thermal-run.sh <cmd>` in tmux; `off` afterwards.
- ivshmem-server running; VMs `ap` and `modem` running (ssh mlink-ap / mlink-modem).
- mlink.ko (Phase 1+2 code) loaded in both VMs: peer discovery verified (ap id 0, modem id 1).

## Done (verified)
P0.1–P0.9 (P0.7 kernel built, P0.8 rootfs, P0.9 both VMs boot < 25 s, ivshmem visible,
/mnt/share mounted). Phase 1+2 driver compiles with -Werror; probe, BAR mapping, MSI-X
registration, header/state publish, peer discovery work in the VMs.

## In progress
- P1.6/P1.7: doorbell vectors + pingpong RTT test (not yet run).
- P0.10: update `scripts/win/test.sh` to insmod with `role=$vm`, run it, tick P0.10.

## Next
1. Run doorbell + pingpong test, record debug-kernel RTT in `bench/results/`.
2. P1.8 reload loop (100× insmod/rmmod each side under traffic) + dmesg gate.
3. P2.3 selftest both directions, 10 M messages, kick suppression on/off; P2.4 unsafe_no_mb.
4. P2.5 litmus tests (herd7 via opam in tmux, or write tests + run later).
5. First GitHub push after P1 tests pass. Then Phase 3 (net_device).

## Open problems / notes
- docs/debugging-log.md has the thermal and concurrent-make stories.
- P0.11 environment.md not written yet.
