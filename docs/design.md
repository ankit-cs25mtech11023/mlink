# mlink design (v1 draft — for review in P1.1)

> Status: **draft, awaiting the user's review** (P1.1). Source: `plan.md` §6. Anything changed
> here that contradicts plan.md is first changed in plan.md.

## 1. Picture

```
  VM "ap"                                         VM "modem"
  ┌──────────────────────────┐                    ┌──────────────────────────┐
  │ TCP/IP stack             │                    │ TCP/IP stack             │
  │ mlink0 (raw IP, NAPI)    │                    │ mlink0 (raw IP, NAPI)    │
  │ mlink.ko role=ap         │                    │ mlink.ko role=modem      │
  │  BAR0 regs  BAR2 shm ────┼──── 4 MiB POSIX ───┼──── BAR2 shm  BAR0 regs  │
  │     │ Doorbell write     │   shared memory    │     ▲ MSI-X vector       │
  └─────┼────────────────────┘                    └─────┼────────────────────┘
        └──── eventfd (ivshmem-server wiring) ──────────┘
```

A "kick" = writing `(peer_id << 16) | vector` to our BAR0 Doorbell register. QEMU signals
the peer's eventfd for that vector, and the peer's QEMU raises that MSI-X vector in the peer
guest. Shared memory carries all data and state; kicks only say "go look".

## 2. Device facts (ivshmem-doorbell, PCI 1af4:1110)

| BAR | What | Notes |
|---|---|---|
| BAR0 | 256 B MMIO: `0x00 IntrMask`, `0x04 IntrStatus` (INTx only), `0x08 IVPosition` (RO, own peer ID), `0x0C Doorbell` (WO) | mapped with `pcim_iomap_region`, accessed with `ioread32/iowrite32` |
| BAR1 | MSI-X table + PBA | used by the PCI core, not by us |
| BAR2 | shared memory (4 MiB) | mapped `devm_memremap(MEMREMAP_WB)` = ordinary cacheable RAM |

- Peer IDs are handed out by ivshmem-server and **change when a VM reconnects**. Each side
  publishes its own ID in its state block, and the other side reads it from there.
- Kicks to a peer that is not connected are silently dropped. The device never reports a
  peer's death → **heartbeat** (§8).

## 3. Roles

One `mlink.ko`, module parameter `role=ap|modem` (required). The **AP is the layout master**:
if the header magic is absent, it writes the header. The modem waits until the header is
valid and matches its own compiled-in layout, then proceeds. The data path is symmetric.

## 4. Shared-memory layout (4 MiB, fixed for the MVP)

| Offset | Size | Region | Written by |
|---|---|---|---|
| `0x000000` | 4 KiB | Global header | AP (once, at init) |
| `0x001000` | 64 B | State block AP | AP only |
| `0x001040` | 64 B | State block MODEM | modem only |
| `0x002000` | 4 KiB | Ring A (AP→MODEM) control (4 cache lines) | split, see §5 |
| `0x003000` | 4 KiB | Ring A descriptors `desc[256]` × 16 B | AP |
| `0x004000` | 1 MiB | Ring A slots `slot[256]` × 4 KiB | AP |
| `0x104000` | 4 KiB | Ring B (MODEM→AP) control | split |
| `0x105000` | 4 KiB | Ring B descriptors | modem |
| `0x106000` | 1 MiB | Ring B slots | modem |
| `0x206000` | ~1.98 MiB | reserved (P7 control rings) | — |

Each ring: N = 256 (power of two), slot size 4096 B → max MTU 4096 (default MTU 1500).
`static_assert` in `mlink_shm.h` pins every offset and size.

### 4.1 Global header
`magic = "MLNK"`, `abi_version`, `ring_entries`, `slot_size`, `ring_a_off`, `ring_b_off`,
`layout_crc` (CRC32 of the fields above). The modem refuses a header whose values differ
from its own constants (version skew between the two builds = refuse, not guess).

### 4.2 State block (one per side, 64 B, one cache line)
`magic`, `peer_id` (own IVPosition), `state` (DOWN/WAIT_PEER/UP), `gen` (bumped on every
(re)start), `peer_gen_seen` (last peer `gen` I acknowledged), `heartbeat` (incremented every
`hb_ms`), `mtu`, `features`.

## 5. Rings

### 5.1 Control block — each line has exactly one writer
| Cache line | Field | Writer (ring A / ring B) | Meaning |
|---|---|---|---|
| 0 | `prod` | AP / modem | free-running count of published descriptors |
| 1 | `cons` | modem / AP | free-running count of consumed descriptors |
| 2 | `prod_flags.want_space` | AP / modem | "producer is stopped, kick SPACE when you free slots" |
| 3 | `cons_flags.want_kick` | modem / AP | "consumer is idle, kick DATA when you publish" |

Separate 64 B lines so the two sides never write to the same cache line (no false sharing,
and the single-writer rule is visible in the layout itself).

### 5.2 Descriptor (16 B)
`u32 len`, `u16 proto` (IPv4/IPv6), `u16 flags`, `u32 seq` (debug/selftest), `u32 rsvd`.
Descriptor `i` always describes slot `i` — no peer-supplied offsets or pointers anywhere.

### 5.3 Indices
`prod`/`cons` are free-running `u32`; slot = `idx & (N-1)`. Used = `prod - cons` (unsigned
wrap-around is correct). Invariant: `prod - cons ≤ N`; anything else = corruption.

## 6. Memory ordering (both sides x86 little-endian for the MVP)

BAR2 is normal WB memory shared with another VM, so we use the `virt_*` barriers
(`virt_store_release`, `virt_load_acquire`, `virt_mb`) and `READ_ONCE/WRITE_ONCE` on every
shared field.

**Publish (producer):** copy payload into slot → write desc → `virt_store_release(&prod, prod+1)`.
**Consume (consumer):** `p = virt_load_acquire(&prod)` → for each entry: `len = READ_ONCE(desc.len)`
**once**, validate, copy out of the slot → `virt_store_release(&cons, cons+1)`.

Release/acquire pairs "payload+desc written" with "payload+desc read" (message-passing
pattern, litmus test `mlink-publish.litmus` in P2.5).

### 6.1 Kick suppression (store-buffering pattern)
```
consumer going idle:                 producer after publishing:
  want_kick = 1                        prod = p + 1          (release)
  virt_mb()                            virt_mb()
  if (prod != cons) poll again         if (want_kick) kick DATA
```
Without **both** full barriers, each side's store can sit in its store buffer while it reads
the other's old value: consumer sees no new data, producer sees `want_kick == 0`, nobody
kicks → lost wakeup, the link stalls. x86 allows exactly this store→load reordering, so this
is a real bug on x86, not a theoretical one (litmus `mlink-kick.litmus`, experiment P2.4).
The TX-space direction (`want_space`) uses the same pattern.

### 6.2 Shared memory is untrusted input
The peer may be buggy or malicious (and P5 corrupts it on purpose). Rules:
1. Read every peer-written field once into a local, then validate the local (no double fetch).
2. `prod - cons > N`, `len > slot_size`, unknown `proto` → count error, drop / request link
   reset. Never crash, never index with an unchecked value.
3. Never use a peer-provided offset or pointer.

## 7. Interrupt vectors

| Vector | Name | Sender → receiver meaning | Receiver action |
|---|---|---|---|
| 0 | DATA | "new descriptors in the ring you consume" | clear `want_kick`, `napi_schedule` |
| 1 | SPACE | "I freed slots you produce into" | `netif_wake_queue` if space |
| 2 | STATE | "my state block changed" | schedule link work (fast SSR path) |
| 3 | TEST | ping-pong / diagnostics (P1) | modem: kick back; AP: record RTT |

## 8. Link state machine & SSR (detailed in P4.1)

```
 DOWN ──probe──► WAIT_PEER ──two-way gen ack──► UP
                   ▲                              │ hb timeout / peer gen changed /
                   │                              │ STATE kick with state=DOWN
                   └────── RESETTING ◄── PEER_LOST┘
```
- UP requires: valid header, peer `state ≥ WAIT_PEER`, `peer.peer_gen_seen == my.gen` and
  `my.peer_gen_seen == peer.gen` (both sides acknowledged each other's current generation).
- Reset (in the link work item, process context): carrier off → stop queue → `napi_disable`
  → drop in-flight TX → reset only the fields **this side owns** → `WAIT_PEER`.
- Heartbeat `hb_ms` = 100, timeout `hb_timeout_ms` = 500 (module params).

## 9. Teardown order (rmmod / probe failure)
`state = DOWN` + STATE kick → unregister netdev → stop link work (`cancel_delayed_work_sync`)
→ mark `dying`, free IRQs → free MSI-X vectors → unmap (devm). IRQ handlers check `dying`.

## 10. Questions for the review (P1.1)
1. Fixed 4 KiB slots waste memory for small packets (a 64 B ACK uses a 4 KiB slot). Accepted
   for the MVP: simple, and each slot is owned by exactly one descriptor. OK?
2. Copy on both sides (no zero-copy). Two copies per packet; simple and safe against an
   untrusted peer. Measured in P6 (copy cost). OK?
3. N = 256 entries per direction. Ring-size sweep in P6 (64/256/1024 needs a layout param →
   stretch). OK?
