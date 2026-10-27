// SPDX-License-Identifier: GPL-2.0-only
/*
 * mlink modem on Zephyr (P8.2/P8.3, plan.md D19/D20).
 *
 * Takes the modem role of the shared-memory ABI in driver/mlink_shm.h, using
 * the Linux driver's ring code (driver/mlink_ring.c) unchanged. The link state
 * machine mirrors driver/mlink_link.c. The data path either reflects every
 * IPv4 ICMP echo request as a reply and every UDP datagram back to its sender
 * (loopback test mode, default), or with CONFIG_MLINK_MODEM_NET hands packets
 * to the Zephyr IP stack (net.c).
 *
 * One thread runs the link and RX: it sleeps on the five doorbell vectors
 * with a HB_MS timeout, so a kick wakes it at once and the timeout drives the
 * heartbeat and handshake.
 */
#include <string.h>

#include <zephyr/drivers/virtualization/ivshmem.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include "modem.h"

#define HB_MS		50
#define HB_TIMEOUT_MS	500	/* same as the driver's hb_timeout_ms default */
#define RX_BUDGET	64
#define STATS_MS	5000

enum reset_reason { R_HB_TIMEOUT, R_PEER_GEN, R_PEER_DOWN, R_RING };

static const char *const reset_names[] = {
	[R_HB_TIMEOUT] = "heartbeat timeout",
	[R_PEER_GEN]   = "peer restarted (gen changed)",
	[R_PEER_DOWN]  = "peer went down",
	[R_RING]       = "ring error",
};

struct modem m;

static struct k_poll_signal sigs[MLINK_NR_VECTORS];
static struct k_poll_event events[MLINK_NR_VECTORS];
static u8 pkt[MLINK_SLOT_SIZE];	/* private copy: never parse shared memory in place */

void modem_kick(enum mlink_vector vec)
{
	if (vec == MLINK_VEC_DATA)
		m.st.kicks_data++;
	else if (vec == MLINK_VEC_SPACE)
		m.st.kicks_space++;
	else if (vec == MLINK_VEC_STATE)
		m.st.kicks_state++;
	ivshmem_int_peer(m.dev, m.peer_id, vec);
}

/* ---- shared header (AP-owned, validated like mlink_hdr_valid()) ---------- */

/* crc32_le(~0, ...) as the Linux driver computes it: reflected, no final xor. */
static u32 crc32_le_noxor(u32 crc, const u8 *p, size_t len)
{
	while (len--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0xedb88320 & -(crc & 1));
	}
	return crc;
}

static bool hdr_valid(void)
{
	struct mlink_shm_hdr *sh = m.hdr;
	struct mlink_shm_hdr want = {
		.abi_version = MLINK_ABI_VERSION,
		.ring_entries = MLINK_RING_ENTRIES,
		.slot_size = MLINK_SLOT_SIZE,
		.ring_a_off = MLINK_RING_A_OFF,
		.ring_b_off = MLINK_RING_B_OFF,
	};

	want.layout_crc = crc32_le_noxor(~0u, (u8 *)&want + MLINK_HDR_CRC_START,
					 MLINK_HDR_CRC_LEN);
	/* Pairs with the AP's release of magic: the fields below are complete. */
	if (virt_load_acquire(&sh->magic) != MLINK_SHM_MAGIC)
		return false;
	return READ_ONCE(sh->abi_version) == want.abi_version &&
	       READ_ONCE(sh->ring_entries) == want.ring_entries &&
	       READ_ONCE(sh->slot_size) == want.slot_size &&
	       READ_ONCE(sh->ring_a_off) == want.ring_a_off &&
	       READ_ONCE(sh->ring_b_off) == want.ring_b_off &&
	       READ_ONCE(sh->layout_crc) == want.layout_crc;
}

/* ---- link state machine (mirrors driver/mlink_link.c) -------------------- */

static void state_publish(enum mlink_link_state st)
{
	struct mlink_shm_state *s = m.self;

	WRITE_ONCE(s->peer_id, m.own_id);
	WRITE_ONCE(s->mtu, OWN_MTU);
	WRITE_ONCE(s->state, st);
	/* Release: the fields above are visible before (or with) the magic. */
	virt_store_release(&s->magic, MLINK_STATE_MAGIC);
	m.state = st;
}

static void link_new_gen(void)
{
	struct mlink_shm_state *s = m.self;

	/* gen survives in shm across our restarts, so the AP always sees a change. */
	WRITE_ONCE(s->gen, READ_ONCE(s->gen) + 1);
	m.gen = READ_ONCE(s->gen);
	virt_wmb();	/* new gen visible before the zeroed indices */
	/* No producer may be mid-publish while its index is zeroed. */
	k_mutex_lock(&m.tx_lock, K_FOREVER);
	mlink_ring_reset_producer(&m.tx);
	m.tx_seq = 0;
	m.tx_broken = false;
	k_mutex_unlock(&m.tx_lock);
	mlink_ring_reset_consumer(&m.rx);
	/* Stale control indices from an earlier Linux modem would look like corruption. */
	mlink_ring_reset_producer(&m.ctrl_tx);
	mlink_ring_reset_consumer(&m.ctrl_rx);
	m.rx_broken = false;
}

static void link_start(void)
{
	link_new_gen();
	m.hb_seen = READ_ONCE(m.peer->heartbeat);
	m.hb_changed = k_uptime_get();
	state_publish(MLINK_ST_WAIT_PEER);
}

static void link_reset(enum reset_reason why)
{
	printk("mlink-modem: link reset: %s (was %s)\n", reset_names[why],
	       m.state == MLINK_ST_UP ? "UP" : "WAIT_PEER");
	m.st.link_resets++;
	/* Senders check link_up under tx_lock; RX runs in this thread. */
	WRITE_ONCE(m.link_up, false);
	modem_net_carrier(false);
	k_sem_give(&m.space_sem);	/* a sender waiting for space rechecks link_up */
	link_new_gen();
	state_publish(MLINK_ST_WAIT_PEER);
	if (m.peer_found)
		modem_kick(MLINK_VEC_STATE);
}

static void link_bring_up(void)
{
	m.st.link_ups++;
	state_publish(MLINK_ST_UP);
	WRITE_ONCE(m.link_up, true);
	modem_net_carrier(m.data_call);
	modem_kick(MLINK_VEC_STATE);
	printk("mlink-modem: link UP: peer id=%u gen=%u, own gen=%u\n",
	       m.peer_id, m.peer_gen, m.gen);
}

static void link_tick(void)
{
	struct mlink_shm_state *p = m.peer;
	u32 magic, pid, pstate, pgen, pseen, phb;
	bool valid, alive;
	int64_t now = k_uptime_get();

	if (!m.hdr_ok) {
		/* The AP owns the layout; wait until it has published it. */
		if (!hdr_valid())
			return;
		m.hdr_ok = true;
		printk("mlink-modem: shared header valid (abi %u)\n", MLINK_ABI_VERSION);
		link_start();
	}

	WRITE_ONCE(m.self->heartbeat, READ_ONCE(m.self->heartbeat) + 1);

	/* Snapshot the peer block once (untrusted): magic first, with acquire. */
	magic = virt_load_acquire(&p->magic);
	pid = READ_ONCE(p->peer_id);
	pseen = READ_ONCE(p->peer_gen_seen);
	virt_rmb();	/* pairs with the AP's wmb: its ack is never newer than its gen */
	pgen = READ_ONCE(p->gen);
	pstate = READ_ONCE(p->state);
	phb = READ_ONCE(p->heartbeat);

	if (phb != m.hb_seen) {
		m.hb_seen = phb;
		m.hb_changed = now;
	} else if (m.state == MLINK_ST_UP) {
		/* Ring progress by the AP is proof of life too (heartbeat starvation). */
		u32 act = READ_ONCE(m.rx.ctrl->prod.val) ^ (READ_ONCE(m.tx.ctrl->cons.val) << 1);

		if (act != m.peer_activity) {
			m.peer_activity = act;
			m.hb_changed = now;
		}
	}
	alive = now - m.hb_changed < HB_TIMEOUT_MS;
	valid = magic == MLINK_STATE_MAGIC && pid <= UINT16_MAX && pid != m.own_id;

	if (valid && (!m.peer_found || pid != m.peer_id)) {
		m.peer_id = pid;
		m.peer_found = true;
		printk("mlink-modem: peer found: id=%u gen=%u\n", pid, pgen);
	} else if (!valid && m.peer_found) {
		m.peer_found = false;
		printk("mlink-modem: peer gone\n");
	}

	if (m.state == MLINK_ST_UP) {
		if (!valid || (pstate != MLINK_ST_UP && pstate != MLINK_ST_WAIT_PEER))
			link_reset(R_PEER_DOWN);
		else if (pgen != m.peer_gen)
			link_reset(R_PEER_GEN);
		else if (!alive)
			link_reset(R_HB_TIMEOUT);
		else if (m.rx_broken || m.tx_broken)
			link_reset(R_RING);
		return;
	}

	/* WAIT_PEER: acknowledge the AP's current generation... */
	if (valid && alive && (pstate == MLINK_ST_WAIT_PEER || pstate == MLINK_ST_UP)) {
		if (READ_ONCE(m.self->peer_gen_seen) != pgen) {
			m.peer_gen = pgen;
			virt_wmb();	/* our gen/reset are already published; the ack comes after */
			WRITE_ONCE(m.self->peer_gen_seen, pgen);
			modem_kick(MLINK_VEC_STATE);
		}
		/* ...and go UP once it has acknowledged ours. */
		if (pseen == m.gen && m.peer_gen == pgen)
			link_bring_up();
	}
}

/* ---- loopback ------------------------------------------------------------ */

/* RFC 1624 eqn. 3: update a 16-bit one's complement checksum for old -> new. */
static void csum_replace16(u8 *csum, u16 old, u16 new)
{
	u32 s = (u16)~sys_get_be16(csum) + (u16)~old + new;

	s = (s & 0xffff) + (s >> 16);
	s = (s & 0xffff) + (s >> 16);
	sys_put_be16((u16)~s, csum);
}

static void swap_bytes(u8 *a, u8 *b, size_t n)
{
	u8 t[4];

	memcpy(t, a, n);
	memcpy(a, b, n);
	memcpy(b, t, n);
}

/*
 * Turn the packet in pkt[] into its reply in place. Swapping two 16-bit-aligned
 * fields never changes a one's complement sum, so the IP header checksum, the
 * UDP checksum and a CHECKSUM_PARTIAL pseudo-header seed all stay valid.
 * Returns false to drop.
 */
static bool reflect(u32 len, const struct mlink_rx_desc *d)
{
	u32 ihl, tot;
	u8 *l4;

	if (d->proto != MLINK_PROTO_IPV4 || len < 20 || (pkt[0] >> 4) != 4)
		return false;
	ihl = (pkt[0] & 0xf) * 4;
	tot = sys_get_be16(&pkt[2]);
	if (ihl < 20 || tot < ihl || tot > len)
		return false;
	l4 = &pkt[ihl];

	/*
	 * A non-first fragment carries no L4 header: swapping the addresses is
	 * all it needs. The first fragment's L4 checksum covers the whole
	 * datagram, and the incremental update below stays correct for it.
	 */
	if (sys_get_be16(&pkt[6]) & 0x1fff) {
		if (pkt[9] != 1 && pkt[9] != 17)
			return false;
		m.st.frags++;
		goto swap_addr;
	}
	if (tot < ihl + 8)
		return false;

	switch (pkt[9]) {
	case 1:		/* ICMP */
		/* The stack fills ICMP checksums itself; partial would be a bug. */
		if (l4[0] != 8 || l4[1] != 0 || (d->flags & MLINK_DESC_F_CSUM_PARTIAL))
			return false;
		l4[0] = 0;	/* echo request -> echo reply */
		csum_replace16(&l4[2], 0x0800, 0x0000);
		m.st.icmp++;
		break;
	case 17:	/* UDP */
		swap_bytes(&l4[0], &l4[2], 2);
		m.st.udp++;
		break;
	default:
		return false;
	}
swap_addr:
	swap_bytes(&pkt[12], &pkt[16], 4);
	return true;
}

/* ---- data path (the modem-side counterpart of mlink_poll()) -------------- */

/* Returns true if it stopped on the budget and should run again right away. */
static bool data_poll(void)
{
	struct mlink_rx_desc d;
	bool consumed = false, published = false;
	int n, budget = RX_BUDGET;

	mlink_ring_disarm_kick(&m.rx);	/* we are polling: no DATA kicks needed */
	while (budget) {
		n = mlink_ring_peek(&m.rx, &d);
		if (n == 0) {
			/* Going idle: arm the kick, then re-check (SB pattern). */
			if (!mlink_ring_arm_kick(&m.rx, true))
				break;
			mlink_ring_disarm_kick(&m.rx);
			continue;
		}
		if (n == -EIO) {
			m.rx_broken = true;
			break;
		}
		if (n == -EMSGSIZE) {
			m.st.bad_len++;
			goto consume;
		}
		if (!READ_ONCE(m.data_call)) {
			/* "Data call" down (SET_DATA_CALL from the AP): no data service. */
			m.st.dc_dropped++;
			goto consume;
		}
		m.st.rx_bytes += d.len;

		if (IS_ENABLED(CONFIG_MLINK_MODEM_NET)) {
			m.st.rx++;
			memcpy(pkt, d.data, d.len);	/* single fetch of the payload */
			if (modem_net_rx(pkt, d.len, &d))
				m.st.to_stack++;
			else
				m.st.dropped++;
			goto consume;
		}

		/*
		 * Loopback: this thread is ring B's only producer, and the reset
		 * runs in this thread too, so no tx_lock is needed here.
		 */
		n = mlink_ring_free(&m.tx);
		if (n < 0) {
			m.tx_broken = true;
			break;
		}
		if (n == 0) {
			/* Ring B full: leave this packet in ring A until the AP frees a slot. */
			if (!mlink_ring_arm_space(&m.tx, true)) {
				m.st.tx_full++;
				break;
			}
			mlink_ring_disarm_space(&m.tx);
		}

		m.st.rx++;
		memcpy(pkt, d.data, d.len);	/* single fetch of the payload */
		if (reflect(d.len, &d)) {
			struct mlink_tx_meta meta = {
				.proto = d.proto,
				.flags = d.flags & MLINK_DESC_F_CSUM_PARTIAL,
				.csum_start = d.csum_start,
				.csum_offset = d.csum_offset,
			};

			memcpy(mlink_ring_slot(&m.tx), pkt, d.len);
			mlink_ring_publish(&m.tx, d.len, &meta, m.tx_seq++);
			published = true;
			m.st.tx++;
			m.st.tx_bytes += d.len;
		} else {
			m.st.dropped++;
		}
consume:
		mlink_ring_consume(&m.rx);
		consumed = true;
		budget--;
	}

	if (consumed) {
		mlink_ring_release(&m.rx);
		if (mlink_ring_need_space_kick(&m.rx, true))
			modem_kick(MLINK_VEC_SPACE);
	}
	if (published && mlink_ring_need_kick(&m.tx, true))
		modem_kick(MLINK_VEC_DATA);
	return budget == 0;
}

/* ---- main ---------------------------------------------------------------- */

static void print_stats(void)
{
	struct modem_stats *s = &m.st;

	printk("mlink-modem: %s gen=%u rx=%u tx=%u icmp=%u udp=%u frag=%u stack=%u csum=%u "
	       "drop=%u badlen=%u full=%u txwait=%u txdrop=%u irq=%u/%u/%u/%u/%u "
	       "kicks d/s/st=%u/%u/%u ups=%u resets=%u ctrl req/resp/inv/full=%u/%u/%u/%u "
	       "dcdrop=%u\n",
	       m.state == MLINK_ST_UP ? "UP" : "WAIT", m.gen, s->rx, s->tx, s->icmp, s->udp,
	       s->frags, s->to_stack, s->csum_fixed, s->dropped, s->bad_len, s->tx_full,
	       s->tx_waits, s->tx_dropped, s->irqs[0], s->irqs[1], s->irqs[2],
	       s->irqs[3], s->irqs[4], s->kicks_data, s->kicks_space, s->kicks_state,
	       s->link_ups, s->link_resets, s->ctrl_req, s->ctrl_resp, s->ctrl_invalid,
	       s->ctrl_full, s->dc_dropped);
}

int main(void)
{
	uintptr_t base;
	size_t size;
	int64_t next_hb, next_stats;
	struct modem_stats last = {0};
	bool busy = false;

	k_mutex_init(&m.tx_lock);
	k_sem_init(&m.space_sem, 0, 1);
	m.data_call = true;
	m.dev = DEVICE_DT_GET(DT_NODELABEL(ivshmem0));
	if (!device_is_ready(m.dev)) {
		printk("mlink-modem: no ivshmem device\n");
		return 0;
	}
	size = ivshmem_get_mem(m.dev, &base);
	if (size < MLINK_LAYOUT_END ||
	    ivshmem_get_vectors(m.dev) < MLINK_NR_VECTORS) {
		printk("mlink-modem: need %u bytes / %u vectors, have %zu / %u\n",
		       MLINK_LAYOUT_END, MLINK_NR_VECTORS, size, ivshmem_get_vectors(m.dev));
		return 0;
	}
	m.shm = (u8 *)base;
	m.hdr = (struct mlink_shm_hdr *)(m.shm + MLINK_HDR_OFF);
	m.self = (struct mlink_shm_state *)(m.shm + MLINK_STATE_MODEM_OFF);
	m.peer = (struct mlink_shm_state *)(m.shm + MLINK_STATE_AP_OFF);
	m.own_id = ivshmem_get_id(m.dev);
	mlink_ring_attach(&m.tx, m.shm + MLINK_RING_B_OFF, MLINK_RING_ENTRIES, MLINK_SLOT_SIZE);
	mlink_ring_attach(&m.rx, m.shm + MLINK_RING_A_OFF, MLINK_RING_ENTRIES, MLINK_SLOT_SIZE);
	mlink_ring_attach(&m.ctrl_tx, m.shm + MLINK_CTRL_D_OFF, MLINK_CTRL_ENTRIES,
			  MLINK_CTRL_SLOT_SIZE);
	mlink_ring_attach(&m.ctrl_rx, m.shm + MLINK_CTRL_C_OFF, MLINK_CTRL_ENTRIES,
			  MLINK_CTRL_SLOT_SIZE);

	for (int i = 0; i < MLINK_NR_VECTORS; i++) {
		k_poll_signal_init(&sigs[i]);
		k_poll_event_init(&events[i], K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY,
				  &sigs[i]);
		ivshmem_register_handler(m.dev, &sigs[i], i);
	}
	printk("mlink-modem: role=modem own_id=%u shm=%zu bytes abi=%u\n",
	       m.own_id, size, MLINK_ABI_VERSION);

	next_hb = next_stats = k_uptime_get();
	for (;;) {
		int64_t now = k_uptime_get();

		k_poll(events, MLINK_NR_VECTORS,
		       busy ? K_NO_WAIT : K_MSEC(MAX(next_hb - now, 0)));
		for (int i = 0; i < MLINK_NR_VECTORS; i++) {
			if (events[i].state == K_POLL_STATE_SIGNALED) {
				m.st.irqs[i]++;
				k_poll_signal_reset(&sigs[i]);
				events[i].state = K_POLL_STATE_NOT_READY;
				/* A sender blocked on a full ring B (IP-stack mode) may go on. */
				if (i == MLINK_VEC_SPACE)
					k_sem_give(&m.space_sem);
			}
		}

		now = k_uptime_get();
		if (now >= next_hb) {
			link_tick();
			next_hb = now + HB_MS;
		}
		/* Any wakeup (DATA, SPACE, STATE, CTRL or timeout) may have work for us. */
		busy = m.link_up && data_poll();
		if (m.link_up)
			modem_ctrl_poll();

		if (now >= next_stats) {
			if (memcmp(&last, &m.st, sizeof(last))) {
				print_stats();
				last = m.st;
			}
			next_stats = now + STATS_MS;
		}
	}
	return 0;
}
