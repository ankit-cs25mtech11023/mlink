# Phase 7 walkthrough — QMI-like control plane (`/dev/mlink_ctrl`)

Files: `driver/mlink_ctrl.c`, `driver/mlink_ctrl.h` (shared with userspace),
`tools/mlinkd.c`, `tools/mlinkctl.c`, `tools/mlink_ctrl_lib.h`, generic rings in
`driver/mlink_ring.c`, `bench/results/p7-ctrl-debug.txt`, plan D18.

---

## 1. Why a control plane

On a phone, the data path (IP packets) is separate from control (QMI services: "start a data
call", "what's the signal", "modem restarted"). Linux exposes QMI to userspace through a
character device (`/dev/cdc-wdm*`, or `qrtr` sockets); a daemon (ModemManager / `qmicli`)
talks requests and gets responses and indications. mlink reproduces that shape.

## 2. Transport: two more rings + one more vector

- Control rings C (AP→modem) and D (modem→AP) live in the previously reserved shm area
  (ABI v3). They reuse the ring code, generalised to per-ring `entries`/`slot_size`
  (16 × 1 KiB here vs 256 × 4 KiB for data).
- One new MSI-X vector **CTRL** (QEMU `vectors=5`, server `-n 5`) means both "new message"
  and "space freed". Control traffic is tiny, so the producer kicks after every publish and
  the consumer after every batch — no kick suppression, therefore no lost-wakeup race to get
  wrong. Writers waiting for space also use a 1 s timeout as a backstop.

## 3. Message format (`mlink_ctrl.h`)

Header `{msg_id, type REQ/RESP/IND, txn_id, len, result}` + TLVs `{type, len, value}`.
- **REQ** — AP → modem; the **AP kernel assigns `txn_id`** (userspace can't spoof another
  process's transaction).
- **RESP** — modem → AP, echoes `txn_id`; the kernel delivers it **only to the file that sent
  the request** (txn table: 32 entries; full → `-EBUSY`).
- **IND** — unsolicited modem → AP, delivered to **every** open file.
- Validation on receive: proto must be CTRL, `len ≥ header`, `header + hdr.len == desc len`,
  known type — checked on our *copy* (memcpy first).

## 4. The misc device

| op | behaviour |
|---|---|
| `write` | validate header; AP: only REQ, kernel sets txn_id; modem: RESP/IND only; ring full → block (or `EAGAIN` with `O_NONBLOCK`); link down → `ENOTCONN` |
| `read` | one whole message per read; buffer too small → `EMSGSIZE` (message stays queued); empty → block / `EAGAIN`; link down and nothing queued → `ENOTCONN` |
| `poll` | `EPOLLIN` queued message, `EPOLLOUT` ring space, `EPOLLHUP` link down, `+EPOLLERR` device gone |
| `ioctl MLINK_IOC_GET_INFO` | role, link state, generations, peer IDs |

Per-file receive queue bounded at 64 messages (oldest dropped, counted): a client that never
reads can't pin unbounded kernel memory.

## 5. Concurrency and lifetime — the parts to understand cold

- **One mutex** protects the control TX ring (many writers → still a single producer), the
  RX consumer work, the txn table and the file list. The consumer runs in a work item
  (process context), so `GFP_KERNEL` and a mutex are fine.
- **Lifetime:** an open file can outlive the PCI device (sysfs unbind). The control state is
  `kzalloc`ed with a **kref** (one ref per file + one for the device). On remove:
  `misc_deregister` (no new opens) → `cancel_work_sync` → under the lock set `dead`, wake
  everyone → drop the device's ref. Every file op locks and checks `dead` first → `ENODEV`;
  the last `close` frees the struct. Tested: unbind with an open fd → read/write/ioctl all
  `ENODEV`, close fine, KASAN clean.
- Module refcount: an open file pins the module (`fops.owner`), so `rmmod` fails while
  `mlinkd` runs — a modem restart restarts its daemon too.

## 6. SSR semantics

On every link reset (inside `mlink_link_new_gen`, after the gen bump):
pending requests get a **synthetic RESP with `result = -ENOTCONN`** queued to their owners,
the control rings are zeroed like the data rings, and readers are woken → `poll` reports
`EPOLLHUP`, `read` returns `ENOTCONN` once the queue is drained. On link UP readers are woken
again and the RX work drains anything already queued.
Tested: a running `mlinkctl bench` and `mlinkctl monitor` both got `ENOTCONN` immediately
when the modem driver restarted (no hang); a restarted `mlinkd` answered again.

## 7. Userspace

- `mlinkd` (modem): `GET_LINK_INFO`, `GET_STATS` (from sysfs), `SET_DATA_CALL` → bring the
  modem's `mlink0` up/down with `SIOCSIFFLAGS`, answer, then send `IND_DATA_CALL`.
- `mlinkctl` (AP): `info`, `stats`, `call up|down`, `monitor`, `bench N`.
- Debug-kernel control round trip (kernel → ring → modem kernel → daemon → back): p50 0.6 ms.

## Likely quiz questions
1. Why does the kernel, not userspace, assign transaction ids?
2. Why is it OK for the control rings to kick on every message but not the data rings?
3. Walk through what happens to a blocked `read()` when the modem restarts.
4. Why a kref, and what bug would a plain `devm_kzalloc` for the control state cause?
5. What does `misc_deregister` guarantee and what does it not?
6. Why is the per-file queue bounded? What is dropped and why that one?
7. Spot-the-bug candidates: routing RESP by `msg_id` instead of `txn_id`; validating
   `hdr.len` from shm and then `memcpy`-ing using the shm value; freeing the control struct in
   `remove()` while a file is open.
