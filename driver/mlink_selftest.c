// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ring consumer work and the ring selftest (P2.3).
 *
 * The producer pushes sequence-numbered messages whose length and payload are
 * derived from the sequence number with xorshift32, so the consumer can verify
 * order, length and content without any side channel.
 */
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/sched.h>

#include "mlink.h"

#define MLINK_SPACE_TIMEOUT_MS	100
#define MLINK_SPACE_MAX_TIMEOUTS 50	/* 5 s without space: peer gone */

static u32 xs32(u32 x)
{
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return x;
}

static u32 st_seed(u32 seq)
{
	u32 x = seq * 2654435761u ^ 0xa5a5a5a5u;

	return x ? x : 1;	/* xorshift must not start at 0 */
}

/* 16..4096 bytes, multiple of 4 */
static u32 st_len(u32 seq)
{
	return 4 * (4 + xs32(st_seed(seq)) % (MLINK_SLOT_SIZE / 4 - 3));
}

static void st_fill(u32 *p, u32 len, u32 seq)
{
	u32 x = st_seed(seq), i;

	for (i = 0; i < len / 4; i++) {
		x = xs32(x);
		p[i] = x;
	}
}

static bool st_check(const u32 *p, u32 len, u32 seq)
{
	u32 x = st_seed(seq), i;

	for (i = 0; i < len / 4; i++) {
		x = xs32(x);
		if (READ_ONCE(p[i]) != x)
			return false;
	}
	return true;
}

/* ---- consumer ------------------------------------------------------------ */

static void mlink_selftest_rx(struct mlink_dev *mdev, const struct mlink_rx_desc *d)
{
	struct mlink_selftest *st = &mdev->st;
	u64 now = ktime_get_ns();

	if (d->seq == 0) {
		/* A new run starts: reset the verifier. */
		st->rx_expect = 0;
		st->rx_ok = st->rx_lost = st->rx_dup_ooo = st->rx_corrupt = 0;
		st->rx_first_ns = now;
	}

	if (d->seq > st->rx_expect)
		st->rx_lost += d->seq - st->rx_expect;
	else if (d->seq < st->rx_expect)
		st->rx_dup_ooo++;

	if (d->len != st_len(d->seq) || !st_check((const u32 *)d->data, d->len, d->seq))
		st->rx_corrupt++;
	else
		st->rx_ok++;

	if (d->seq >= st->rx_expect)
		st->rx_expect = d->seq + 1;
	st->rx_last_ns = now;
}

/* Consume up to 'budget' messages. Returns the number consumed. */
int mlink_rx_poll(struct mlink_dev *mdev, int budget)
{
	struct mlink_ring *r = &mdev->rx;
	struct mlink_rx_desc d;
	int done = 0, ret;

	while (done < budget) {
		ret = mlink_ring_peek(r, &d);
		if (!ret)
			break;
		if (ret == -EIO) {
			mdev->stats.ring_corruptions++;
			WRITE_ONCE(mdev->rx_broken, true);
			dev_err_ratelimited(&mdev->pdev->dev,
					    "rx ring corrupt: prod-cons > %u, stopping rx\n",
					    MLINK_RING_ENTRIES);
			break;
		}
		if (ret == -EMSGSIZE)
			mdev->stats.rx_len_errors++;
		else if (d.proto == MLINK_PROTO_TEST)
			mlink_selftest_rx(mdev, &d);
		else
			mdev->stats.rx_proto_errors++;	/* P3: hand IP packets to the stack */

		mlink_ring_consume(r);
		done++;
	}

	if (done) {
		mdev->stats.rx_msgs += done;
		mlink_ring_release(r);
		if (mlink_ring_need_space_kick(r, !unsafe_no_mb)) {
			mlink_kick(mdev, MLINK_VEC_SPACE);
			mdev->stats.space_kicks++;
		}
	}
	return done;
}

void mlink_rx_work(struct work_struct *work)
{
	struct mlink_dev *mdev = container_of(work, struct mlink_dev, rx_work);
	int done;

	if (READ_ONCE(mdev->dying) || READ_ONCE(mdev->rx_broken))
		return;

	done = mlink_rx_poll(mdev, MLINK_RX_BUDGET);
	if (done == MLINK_RX_BUDGET) {
		/* More work: requeue instead of looping, so others get the CPU. */
		queue_work(system_highpri_wq, &mdev->rx_work);
		return;
	}
	/* Going idle: arm the DATA kick, then re-check (lost-wakeup race). */
	if (mlink_ring_arm_kick(&mdev->rx, !unsafe_no_mb)) {
		mlink_ring_disarm_kick(&mdev->rx);
		queue_work(system_highpri_wq, &mdev->rx_work);
	}
}

/* ---- producer ------------------------------------------------------------ */

static int mlink_tx_wait_space(struct mlink_dev *mdev)
{
	struct mlink_ring *r = &mdev->tx;
	int free = mlink_ring_free(r), timeouts = 0;
	long irqs;

	if (free < 0)
		return -EIO;
	if (free)
		return 0;

	mdev->stats.tx_space_waits++;
	mdev->st.tx_space_waits++;
	for (;;) {
		irqs = atomic_long_read(&mdev->irq_count[MLINK_VEC_SPACE]);
		if (mlink_ring_arm_space(r, !unsafe_no_mb))
			break;
		/* Armed and still full: the consumer now owes us a SPACE kick. */
		if (wait_event_timeout(mdev->tx_wq,
				       mlink_ring_free(r) || READ_ONCE(mdev->dying),
				       msecs_to_jiffies(MLINK_SPACE_TIMEOUT_MS)))
			break;
		if (mlink_ring_free(r)) {
			/* Space without a SPACE irq: the kick was lost. */
			if (atomic_long_read(&mdev->irq_count[MLINK_VEC_SPACE]) == irqs) {
				mdev->stats.tx_lost_space_wakeups++;
				mdev->st.tx_lost_wakeups++;
			}
			break;
		}
		mdev->stats.tx_space_timeouts++;
		if (++timeouts >= MLINK_SPACE_MAX_TIMEOUTS) {
			mlink_ring_disarm_space(r);
			return -ETIMEDOUT;
		}
	}
	mlink_ring_disarm_space(r);
	if (READ_ONCE(mdev->dying))
		return -ESHUTDOWN;
	free = mlink_ring_free(r);
	return free < 0 ? -EIO : 0;
}

/* Push n selftest messages as fast as the ring allows. Process context. */
int mlink_selftest_tx(struct mlink_dev *mdev, u32 n)
{
	struct mlink_selftest *st = &mdev->st;
	struct mlink_ring *r = &mdev->tx;
	u64 t0, kicks = 0, supp = 0;
	u32 seq;
	int ret = 0;

	if (!READ_ONCE(mdev->peer_found))
		return -ENOTCONN;

	mutex_lock(&st->lock);
	st->tx_space_waits = 0;
	st->tx_lost_wakeups = 0;
	t0 = ktime_get_ns();
	for (seq = 0; seq < n; seq++) {
		u32 len = st_len(seq);

		ret = mlink_tx_wait_space(mdev);
		if (ret)
			break;
		st_fill(mlink_ring_slot(r), len, seq);
		mlink_ring_publish(r, len, MLINK_PROTO_TEST, seq);
		if (!kick_suppress || mlink_ring_need_kick(r, !unsafe_no_mb)) {
			mlink_kick(mdev, MLINK_VEC_DATA);
			kicks++;
		} else {
			supp++;
		}
		if (!(seq & 255))
			cond_resched();
	}
	st->tx_ns = ktime_get_ns() - t0;
	st->tx_sent = seq;
	st->tx_err = ret;
	st->tx_kicks = kicks;
	st->tx_suppressed = supp;
	mdev->stats.tx_kicks += kicks;
	mdev->stats.tx_kick_suppressed += supp;
	mutex_unlock(&st->lock);
	return ret;
}
