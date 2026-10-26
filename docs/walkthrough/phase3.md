# Phase 3 walkthrough — the network device

Files: `driver/mlink_net.c`, `driver/mlink_trace.h`, NAPI/xmit parts of `mlink_main.c`,
`scripts/guest/mlink-up.sh`, `scripts/win/test.sh`, `bench/results/p3-*.txt`.

---

## 1. What kind of netdev, and why

`mlink_setup()` copies the modem convention of `drivers/net/mhi_net.c` (checked in the 6.18
tree): `type = ARPHRD_RAWIP` (519), `IFF_POINTOPOINT | IFF_NOARP`, `header_ops = NULL`,
`hard_header_len = 0`, `addr_len = 0`. A modem data channel carries bare IP packets — there is
no Ethernet header and nobody to ARP for. `ip link` shows `link/[519]`.
Addresses: `ip addr add 10.77.0.1 peer 10.77.0.2 dev mlink0` → a /32 host route to the peer.

`alloc_netdev(sizeof(struct mlink_dev *), "mlink%d", NET_NAME_ENUM, mlink_setup)`: the private
area just holds a pointer back to our `mlink_dev` (which devres owns), so the netdev and the
PCI device have independent lifetimes.

## 2. TX path: `mlink_start_xmit()`

Context: called by the qdisc layer with the TX queue lock held (BHs off) → we are the **only
producer** of the TX ring, no extra lock needed. Can't sleep.

1. Map `skb->protocol` → `MLINK_PROTO_IPV4/6`; anything else is dropped (raw-IP link).
2. `free = mlink_ring_free()` (acquire of the peer's `cons`).
   - `< 0` → impossible index → `tx_broken`, poke the link work (Phase 4 decides restart vs
     corruption), drop.
   - `== 0` while the queue was running (only after ifup with a full ring) → stop queue, arm
     `want_space`, re-check, return **`NETDEV_TX_BUSY`** (core requeues the skb). Found by
     the soak: the first version dropped here (215 drops).
3. `skb_copy_bits()` into the slot — handles linear + paged skbs. **Copy, not zero-copy**: the
   peer must never get pointers into our memory, and the slot is reusable as soon as it's
   consumed.
4. `mlink_ring_publish()` (release of `prod`), stats under `u64_stats_update_begin/end`,
   `dev_consume_skb_any()` ("consume", not "kfree": not a drop, so drop monitors stay quiet).
5. If this used the last slot: `netif_stop_queue` → arm `want_space` (+mb) → re-check → wake
   if space appeared. Same SB pattern as the DATA kick; without the re-check a SPACE kick that
   raced with the stop would be lost → **NETDEV WATCHDOG: transmit queue 0 timed out**.
6. `netdev_xmit_more()`: the stack says another packet follows immediately → skip the kick,
   one kick per batch. Then the suppressed-kick decision (`need_kick`, full barrier).

## 3. RX path: IRQ → NAPI

- DATA IRQ (hard IRQ): `disarm want_kick` (further kicks would just be wasted interrupts) →
  `napi_schedule_irqoff()`. Interrupt mitigation in two lines.
- `mlink_napi_poll(napi, budget)` runs in **softirq** context (NET_RX_SOFTIRQ; can't sleep,
  `GFP_ATOMIC`-only allocations): `mlink_rx_poll()` consumes ≤ budget descriptors.
- Per IP descriptor, `mlink_net_rx()`: `napi_alloc_skb(len)` (per-CPU page-frag cache, cheap
  in NAPI) → `skb_put_data()` copies **out of shm** → classify from `skb->data[0] >> 4`
  (the *private copy* — never re-read shm) → `skb->protocol`, `skb_reset_network_header`,
  stats → `napi_gro_receive()` (GRO merges TCP segments before the stack).
- After the loop: one `release` (store-release of `cons`) for the whole batch, then
  `need_space_kick` (mb) → SPACE kick if the producer is stopped.
- `done == budget` → return budget: the core keeps us scheduled (fairness with other NAPIs).
- `done < budget` → `napi_complete_done()`; if it returns true, arm `want_kick` (+mb) and
  re-check `prod`; data arrived meanwhile → `napi_schedule` again. That re-check is the
  consumer half of the lost-wakeup race.

## 4. Who may produce on the TX ring

SPSC means one producer. Both `ndo_start_xmit` and the ring selftest want to produce, so
`tx_owner` (`atomic_cmpxchg`) admits only one: `ndo_open` takes it (else `-EBUSY`), the
selftest takes it (else `-EBUSY`), `ndo_stop` / selftest end release it. The core guarantees
no `xmit` runs after `ndo_stop` returns.

## 5. Stats

- `ndo_get_stats64` reads two `struct mlink_netstats` (rx written only by NAPI, tx only by
  xmit) with `u64_stats_fetch_begin/retry` — on 64-bit this compiles to nothing; on 32-bit it
  is a seqcount so a reader never sees a torn 64-bit value.
- Driver counters (`ethtool -S mlink0`): kicks, suppressed kicks, queue stops/wakes, NAPI
  polls, budget exhaustion, errors, stalls, link resets — the evidence for every claim.

## 6. Tracepoints

`TRACE_EVENT`s in `mlink_trace.h`; `CREATE_TRACE_POINTS` in exactly one .c file
(`mlink_main.c`); `TRACE_INCLUDE_PATH .` + `ccflags-y += -I$(src)` because the header is not
under `include/trace/events/`. Near-zero cost when disabled (static keys).
`trace-cmd record -e mlink ping -c 2 10.77.0.2` showed a real latency effect: an interrupt
that arrives during a softirq gets its NET_RX processing deferred to **ksoftirqd** (+2.3 ms in
that sample) — a scheduler-latency effect, not a ring problem.

## 7. Results (debug kernel — label as such)
- ping both ways, TCP/UDP both ways, `-P 4`, IPv6: all pass; UDP 0/81k datagrams lost; tx(ap)
  == rx(modem) exactly (`p3-functional-debug.txt`).
- TCP ≈ 47 Mbit/s: **sender CPU-bound** (iperf3 at 99 % sys under KASAN), the link is idle
  most of the time. Ping ≈ 1 ms: two debug network stacks + ksoftirqd deferral; the link's
  own interrupt round trip is ~10–25 µs.
- 10-min soak (bidir `-P 8` + ping flood + 105 link flaps): dmesg gate clean, 0 stalls, 0
  corruptions, no watchdog timeouts (`p3-soak-debug.txt`).

## Likely quiz questions
1. Why `ARPHRD_RAWIP` + `IFF_NOARP`? What would break with `ether_setup`?
2. In which context does `ndo_start_xmit` run? NAPI poll? Can either sleep?
3. Walk a packet from `send()` on the AP to `recv()` on the modem (explain-back exercise).
4. Why is the stop-queue path followed by a re-check? What's the symptom if it's missing?
5. What does `napi_complete_done()` returning false mean?
6. Why classify the IP version from the skb and not from the descriptor or shm?
7. What does `xmit_more` buy? What would go wrong if the last packet of a batch also skipped
   the kick?
8. Spot-the-bug candidates: `napi_complete_done` followed by return without the re-arm
   re-check; `GFP_KERNEL` allocation in the poll; `dev_kfree_skb` vs `dev_consume_skb_any`.
