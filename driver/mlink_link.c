// SPDX-License-Identifier: GPL-2.0-only
/*
 * Link state machine, heartbeat and Subsystem Restart (design.md §8).
 *
 *   DOWN --probe--> WAIT_PEER --two-way gen ack + live heartbeat--> UP
 *                       ^                                           |
 *                       +---- reset (bump gen, zero own fields) <---+
 *        peer heartbeat stale | peer gen changed | peer DOWN | ring error
 *
 * All transitions run in the link work item (process context, one instance
 * at a time), which is the only place that resets or brings the link up. The
 * data path (xmit, NAPI) never takes a lock for the link state: it is stopped
 * (netif_tx_disable, napi_disable) before anything it uses is reset.
 */
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/netdevice.h>

#include "mlink.h"
#include "mlink_trace.h"

static const char * const mlink_reset_names[] = {
	[MLINK_RESET_HB_TIMEOUT]  = "heartbeat timeout",
	[MLINK_RESET_PEER_GEN]    = "peer restarted (gen changed)",
	[MLINK_RESET_PEER_DOWN]   = "peer went down",
	[MLINK_RESET_RING]        = "ring error",
};

static void mlink_state_publish(struct mlink_dev *mdev, enum mlink_link_state st)
{
	struct mlink_shm_state *s = mdev->self;

	WRITE_ONCE(s->peer_id, mdev->own_id);
	WRITE_ONCE(s->mtu, mdev->ndev->mtu);
	WRITE_ONCE(s->state, st);
	/* Release: the fields above are visible before (or with) the magic. */
	virt_store_release(&s->magic, MLINK_STATE_MAGIC);
	WRITE_ONCE(mdev->link_state, st);
}

/*
 * Start a new incarnation of our side: new gen first, then zero the ring
 * fields we own. A peer that sees our zeroed index (prod - cons > N) also
 * sees the new gen and treats it as a restart, not corruption.
 */
static void mlink_link_new_gen(struct mlink_dev *mdev)
{
	struct mlink_shm_state *s = mdev->self;

	WRITE_ONCE(s->gen, READ_ONCE(s->gen) + 1);
	mdev->gen = READ_ONCE(s->gen);
	virt_wmb();	/* new gen visible before the zeroed indices */
	mlink_ring_reset_producer(&mdev->tx);
	mlink_ring_reset_consumer(&mdev->rx);
	/* Control rings too; pending requests fail with -ENOTCONN (P7). */
	mlink_ctrl_link_reset(mdev);
	mdev->tx_seq = 0;
	/*
	 * Both error latches belong to the old incarnation (a stale tx_broken
	 * caused an endless reset loop under TCP in P4.5).
	 */
	WRITE_ONCE(mdev->rx_broken, false);
	WRITE_ONCE(mdev->tx_broken, false);
	mdev->rx_stall_seen = 0;
}

/* Called once from probe (or from the link work once the modem sees the header). */
void mlink_link_start(struct mlink_dev *mdev)
{
	mlink_link_new_gen(mdev);
	mdev->hb_seen = READ_ONCE(mdev->peer->heartbeat);
	mdev->hb_changed = jiffies;
	mlink_state_publish(mdev, MLINK_ST_WAIT_PEER);
}

static void mlink_link_reset(struct mlink_dev *mdev, enum mlink_reset_reason why)
{
	struct net_device *ndev = mdev->ndev;
	bool was_up = mdev->link_state == MLINK_ST_UP;

	mdev->t_detect = ktime_get();
	/* Detection latency for a dead peer: how stale its heartbeat was. */
	mdev->last_hb_age_ms = jiffies_to_msecs(jiffies - mdev->hb_changed);
	mdev->stats.link_resets++;
	mdev->reset_count[why]++;
	if (why == MLINK_RESET_RING)
		mdev->stats.ring_corruptions++;	/* bad index, peer gen unchanged */
	dev_info(&mdev->pdev->dev, "link reset: %s (was %s)\n", mlink_reset_names[why],
		 was_up ? "UP" : "WAIT_PEER");
	if (why == MLINK_RESET_RING)
		dev_info(&mdev->pdev->dev,
			 "ring error: tx_broken=%d idx=%u peer_cons=%u | rx_broken=%d idx=%u peer_prod=%u | peer gen=%u acked=%u\n",
			 READ_ONCE(mdev->tx_broken), mdev->tx.idx, mdev->tx.seen,
			 READ_ONCE(mdev->rx_broken), mdev->rx.idx, mdev->rx.seen,
			 READ_ONCE(mdev->peer->gen), mdev->peer_gen);
	trace_mlink_link(mdev->link_state, MLINK_ST_WAIT_PEER, why);

	/*
	 * Quiesce the data path before touching anything it uses. link_up goes
	 * false FIRST: the SPACE irq wakes the TX queue only while it is true,
	 * and synchronize_irq() waits out a handler that saw the old value.
	 * Otherwise a wake between netif_tx_disable() and the index reset lets
	 * xmit race mlink_link_new_gen() (found in P4.5: reset loop under TCP).
	 */
	WRITE_ONCE(mdev->link_up, false);
	if (was_up) {
		netif_carrier_off(ndev);
		synchronize_irq(pci_irq_vector(mdev->pdev, MLINK_VEC_SPACE));
		netif_tx_disable(ndev);		/* waits for a running xmit */
		napi_disable(&mdev->napi);	/* waits for a running poll */
	}
	wake_up(&mdev->tx_wq);
	mutex_lock(&mdev->st.lock);	/* a selftest producer sees !link_up and stops */
	mlink_link_new_gen(mdev);
	mutex_unlock(&mdev->st.lock);

	mlink_state_publish(mdev, MLINK_ST_WAIT_PEER);
	if (READ_ONCE(mdev->peer_found))
		mlink_kick(mdev, MLINK_VEC_STATE);	/* fast path: peer reacts now */
}

static void mlink_link_bring_up(struct mlink_dev *mdev)
{
	struct net_device *ndev = mdev->ndev;

	mdev->stats.link_ups++;
	if (mdev->t_detect) {
		mdev->last_recover_us = ktime_us_delta(ktime_get(), mdev->t_detect);
		mdev->t_detect = 0;
	}
	trace_mlink_link(mdev->link_state, MLINK_ST_UP, 0);
	mlink_state_publish(mdev, MLINK_ST_UP);
	WRITE_ONCE(mdev->link_up, true);

	napi_enable(&mdev->napi);
	/* First poll drains anything already published and arms the DATA kick. */
	local_bh_disable();
	napi_schedule(&mdev->napi);
	local_bh_enable();

	netif_carrier_on(ndev);
	if (netif_running(ndev))
		netif_wake_queue(ndev);
	mlink_ctrl_link_up(mdev);
	mlink_kick(mdev, MLINK_VEC_STATE);
	dev_info(&mdev->pdev->dev, "link UP: peer id=%u gen=%u, own gen=%u (recovery %llu us)\n",
		 mdev->peer_id, mdev->peer_gen, mdev->gen, mdev->last_recover_us);
}

/* Graceful stop (rmmod): tell the peer immediately, before tearing down. */
void mlink_link_stop(struct mlink_dev *mdev)
{
	struct mlink_shm_state *s = mdev->self;

	WRITE_ONCE(s->state, MLINK_ST_DOWN);
	/* Release: a peer that sees the cleared magic also sees DOWN. */
	virt_store_release(&s->magic, 0);
	WRITE_ONCE(mdev->link_state, MLINK_ST_DOWN);
	if (READ_ONCE(mdev->peer_found))
		mlink_kick(mdev, MLINK_VEC_STATE);
}

/* Lost-DATA-kick watchdog; see phase2 walkthrough §7. */
static void mlink_link_rx_watchdog(struct mlink_dev *mdev)
{
	/*
	 * Decide on local facts only: data pending and our consumer index has
	 * not moved for two polls. Do NOT require want_kick == 1: that flag
	 * lives in shm, and shm_chaos "flags" cleared it while we were idle,
	 * so the producer stopped kicking and the old check never fired.
	 */
	if (mlink_ring_pending(&mdev->rx) &&
	    (!mdev->rx_stall_seen || mdev->rx.idx == mdev->rx_stall_idx)) {
		mdev->rx_stall_idx = mdev->rx.idx;
		if (++mdev->rx_stall_seen >= 2) {
			mdev->stats.rx_stalls++;
			mdev->rx_stall_seen = 0;
			mlink_ring_disarm_kick(&mdev->rx);
			local_bh_disable();
			napi_schedule(&mdev->napi);
			local_bh_enable();
		}
	} else {
		mdev->rx_stall_seen = 0;
	}
}

/*
 * Lost-SPACE-kick watchdog for the netdev TX queue: stopped, yet the ring has
 * room on two consecutive polls. Found by shm_chaos "flags" (want_space
 * cleared while the producer waited): the queue stayed stopped forever.
 */
static void mlink_link_tx_watchdog(struct mlink_dev *mdev)
{
	struct net_device *ndev = mdev->ndev;

	if (netif_running(ndev) && netif_queue_stopped(ndev) &&
	    mlink_ring_free(&mdev->tx) > 0) {
		if (++mdev->tx_stall_seen >= 2) {
			mdev->stats.tx_stalls++;
			mdev->tx_stall_seen = 0;
			mlink_ring_disarm_space(&mdev->tx);
			netif_wake_queue(ndev);
		}
	} else {
		mdev->tx_stall_seen = 0;
	}
}

/* UP and healthy: look for lost kicks in both directions. */
static void mlink_link_watchdogs(struct mlink_dev *mdev)
{
	mlink_link_rx_watchdog(mdev);
	mlink_link_tx_watchdog(mdev);
}

void mlink_link_work(struct work_struct *work)
{
	struct mlink_dev *mdev = container_of(to_delayed_work(work),
					      struct mlink_dev, link_work);
	struct device *dev = &mdev->pdev->dev;
	struct mlink_shm_state *p = mdev->peer;
	u32 magic, pid, pstate, pgen, pseen, phb;
	bool valid, alive;

	if (READ_ONCE(mdev->dying))
		return;

	if (!mdev->hdr_ok) {
		/* Modem: the AP owns the layout; wait until it has published it. */
		if (!mlink_hdr_valid(mdev))
			goto again;
		WRITE_ONCE(mdev->hdr_ok, true);
		dev_info(dev, "shared header valid (abi %u)\n", MLINK_ABI_VERSION);
		mlink_link_start(mdev);
	}

	/* AP owns the header: repair it if something corrupted it (P5.1 chaos). */
	if (mdev->role == MLINK_ROLE_AP && !mlink_hdr_valid(mdev)) {
		mdev->stats.hdr_repairs++;
		dev_warn_ratelimited(dev, "shared header corrupted, rewriting\n");
		mlink_hdr_publish(mdev);
	}

	/* Our heartbeat: proof of life for the peer's watchdog. */
	WRITE_ONCE(mdev->self->heartbeat, READ_ONCE(mdev->self->heartbeat) + 1);

	/* Snapshot the peer block once (untrusted): magic first, with acquire. */
	magic = virt_load_acquire(&p->magic);
	pid = READ_ONCE(p->peer_id);
	pseen = READ_ONCE(p->peer_gen_seen);
	virt_rmb();	/* pairs with the peer's wmb: its ack is never newer than its gen */
	pgen = READ_ONCE(p->gen);
	pstate = READ_ONCE(p->state);
	phb = READ_ONCE(p->heartbeat);

	if (phb != mdev->hb_seen) {
		/* Worst observed heartbeat interval while UP: sizes hb_timeout_ms. */
		if (mdev->link_state == MLINK_ST_UP)
			mdev->max_hb_gap_ms = max(mdev->max_hb_gap_ms,
						  jiffies_to_msecs(jiffies - mdev->hb_changed));
		mdev->hb_seen = phb;
		mdev->hb_changed = jiffies;
	} else if (mdev->link_state == MLINK_ST_UP) {
		/*
		 * Under bulk traffic the peer's link work (heartbeat) was seen
		 * starved for seconds while its data path kept running. Ring
		 * progress by the peer is proof of life too; a dead peer makes
		 * none. (debugging-log: "heartbeat starvation")
		 */
		u32 act = READ_ONCE(mdev->rx.ctrl->prod.val) ^
			  (READ_ONCE(mdev->tx.ctrl->cons.val) << 1);

		if (act != mdev->peer_activity) {
			mdev->peer_activity = act;
			mdev->hb_changed = jiffies;
			mdev->stats.hb_saved_by_ring++;
		}
	}
	alive = time_before(jiffies, mdev->hb_changed + msecs_to_jiffies(hb_timeout_ms));
	valid = magic == MLINK_STATE_MAGIC && pid <= U16_MAX && pid != mdev->own_id;

	if (valid && (!mdev->peer_found || pid != mdev->peer_id)) {
		WRITE_ONCE(mdev->peer_id, pid);	/* IDs change when a VM reconnects */
		WRITE_ONCE(mdev->peer_found, true);
		dev_info(dev, "peer found: id=%u gen=%u\n", pid, pgen);
	} else if (!valid && mdev->peer_found) {
		WRITE_ONCE(mdev->peer_found, false);
		dev_info(dev, "peer gone\n");
	}

	if (mdev->link_state == MLINK_ST_UP) {
		/* Anything but WAIT_PEER/UP (DOWN or a corrupted value) = peer down. */
		if (!valid || (pstate != MLINK_ST_UP && pstate != MLINK_ST_WAIT_PEER))
			mlink_link_reset(mdev, MLINK_RESET_PEER_DOWN);
		else if (pgen != mdev->peer_gen)
			mlink_link_reset(mdev, MLINK_RESET_PEER_GEN);
		else if (!alive)
			mlink_link_reset(mdev, MLINK_RESET_HB_TIMEOUT);
		else if (READ_ONCE(mdev->rx_broken) || READ_ONCE(mdev->tx_broken))
			mlink_link_reset(mdev, MLINK_RESET_RING);
		else
			mlink_link_watchdogs(mdev);
		goto again;
	}

	/* WAIT_PEER: acknowledge the peer's current generation... */
	if (valid && alive && (pstate == MLINK_ST_WAIT_PEER || pstate == MLINK_ST_UP)) {
		if (READ_ONCE(mdev->self->peer_gen_seen) != pgen) {
			mdev->peer_gen = pgen;
			/* Our gen/reset are already published; the ack comes after. */
			virt_wmb();
			WRITE_ONCE(mdev->self->peer_gen_seen, pgen);
			mlink_kick(mdev, MLINK_VEC_STATE);
		}
		/* ...and go UP once it has acknowledged ours. */
		if (pseen == mdev->gen && mdev->peer_gen == pgen)
			mlink_link_bring_up(mdev);
	}

again:
	if (!READ_ONCE(mdev->dying))
		mod_delayed_work(system_highpri_wq, &mdev->link_work, msecs_to_jiffies(hb_ms));
}
