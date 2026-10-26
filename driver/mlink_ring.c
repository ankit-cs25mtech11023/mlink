// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPSC ring operations on shared memory (design.md §5-6).
 *
 * Ordering summary:
 *  - publish: payload + desc stores, then store-release(prod)
 *  - peek:    load-acquire(prod), then desc + payload loads
 *  - release: payload loads done, then store-release(cons)
 *  - free:    load-acquire(cons), then the producer may overwrite the slot
 *  - kicks:   store-buffering pattern; each side does store flag/index,
 *             virt_mb(), load the other side's index/flag. 'safe' = false
 *             drops the virt_mb() for the P2.4 lost-wakeup experiment only.
 */
#include <asm/barrier.h>
#include <linux/compiler.h>
#include <linux/errno.h>

#include "mlink_ring.h"

void mlink_ring_attach(struct mlink_ring *r, void *base, bool producer)
{
	r->ctrl = base + MLINK_RING_CTRL_OFF;
	r->desc = base + MLINK_RING_DESC_OFF;
	r->slots = base + MLINK_RING_SLOTS_OFF;
	r->mask = MLINK_RING_ENTRIES - 1;
	r->idx = 0;	/* the link state machine resets the shared fields */
}

/* Zero the producer-owned fields (link reset, data path quiesced). */
void mlink_ring_reset_producer(struct mlink_ring *r)
{
	r->idx = 0;
	WRITE_ONCE(r->ctrl->want_space.val, 0);
	WRITE_ONCE(r->ctrl->prod.val, 0);
}

/* Zero the consumer-owned fields (link reset, data path quiesced). */
void mlink_ring_reset_consumer(struct mlink_ring *r)
{
	r->idx = 0;
	WRITE_ONCE(r->ctrl->want_kick.val, 0);
	WRITE_ONCE(r->ctrl->cons.val, 0);
}

/* ---- producer ------------------------------------------------------------ */

/* Free slots, or -EIO if the peer's cons is impossible. */
int mlink_ring_free(struct mlink_ring *r)
{
	/* Acquire: the consumer finished reading a slot before we refill it. */
	u32 cons = virt_load_acquire(&r->ctrl->cons.val);
	u32 used = r->idx - cons;	/* unsigned wrap-around is intended */

	if (unlikely(used > MLINK_RING_ENTRIES)) {
		r->seen = cons;
		return -EIO;
	}
	return MLINK_RING_ENTRIES - used;
}

void *mlink_ring_slot(struct mlink_ring *r)
{
	return r->slots + (size_t)(r->idx & r->mask) * MLINK_SLOT_SIZE;
}

/* Caller has filled mlink_ring_slot() and checked mlink_ring_free() > 0. */
void mlink_ring_publish(struct mlink_ring *r, u32 len, u16 proto, u32 seq)
{
	struct mlink_desc *d = &r->desc[r->idx & r->mask];

	WRITE_ONCE(d->len, len);
	WRITE_ONCE(d->proto, proto);
	WRITE_ONCE(d->flags, 0);
	WRITE_ONCE(d->seq, seq);
	r->idx++;
	/* Release: payload and descriptor become visible no later than prod. */
	virt_store_release(&r->ctrl->prod.val, r->idx);
}

/* After publishing: does the consumer want a DATA kick? */
bool mlink_ring_need_kick(struct mlink_ring *r, bool safe)
{
	if (safe)
		virt_mb();	/* prod store before want_kick load (SB pattern) */
	return READ_ONCE(r->ctrl->want_kick.val);
}

/* Ring full: ask for a SPACE kick. Returns true if space appeared meanwhile. */
bool mlink_ring_arm_space(struct mlink_ring *r, bool safe)
{
	WRITE_ONCE(r->ctrl->want_space.val, 1);
	if (safe)
		virt_mb();	/* want_space store before cons re-read (SB pattern) */
	return mlink_ring_free(r) != 0;
}

void mlink_ring_disarm_space(struct mlink_ring *r)
{
	WRITE_ONCE(r->ctrl->want_space.val, 0);
}

/* ---- consumer ------------------------------------------------------------ */

/*
 * Look at the next descriptor without consuming it.
 * Returns the number of available entries (> 0), 0 if empty, -EIO if prod is
 * impossible (corruption), -EMSGSIZE if this descriptor's length is invalid
 * (the caller still consumes it to skip it).
 */
int mlink_ring_peek(struct mlink_ring *r, struct mlink_rx_desc *d)
{
	/* Acquire: pairs with the producer's release of prod. */
	u32 prod = virt_load_acquire(&r->ctrl->prod.val);
	u32 avail = prod - r->idx;
	const struct mlink_desc *sd;

	if (!avail)
		return 0;
	if (unlikely(avail > MLINK_RING_ENTRIES)) {
		r->seen = prod;
		return -EIO;
	}

	sd = &r->desc[r->idx & r->mask];
	/* Fetch each peer-written field once; only the local copies are trusted. */
	d->len = READ_ONCE(sd->len);
	d->proto = READ_ONCE(sd->proto);
	d->seq = READ_ONCE(sd->seq);
	d->data = r->slots + (size_t)(r->idx & r->mask) * MLINK_SLOT_SIZE;
	if (unlikely(!d->len || d->len > MLINK_SLOT_SIZE))
		return -EMSGSIZE;
	return avail;
}

void mlink_ring_consume(struct mlink_ring *r)
{
	r->idx++;
}

/* Publish everything consumed so far (batched: one shared store per poll). */
void mlink_ring_release(struct mlink_ring *r)
{
	/* Release: our reads of the slots complete before the producer reuses them. */
	virt_store_release(&r->ctrl->cons.val, r->idx);
}

/* After releasing: does the producer want a SPACE kick? */
bool mlink_ring_need_space_kick(struct mlink_ring *r, bool safe)
{
	if (safe)
		virt_mb();	/* cons store before want_space load (SB pattern) */
	return READ_ONCE(r->ctrl->want_space.val);
}

/* Going idle: ask for a DATA kick. Returns true if data arrived meanwhile. */
bool mlink_ring_arm_kick(struct mlink_ring *r, bool safe)
{
	WRITE_ONCE(r->ctrl->want_kick.val, 1);
	if (safe)
		virt_mb();	/* want_kick store before prod re-read (SB pattern) */
	return READ_ONCE(r->ctrl->prod.val) != r->idx;
}

void mlink_ring_disarm_kick(struct mlink_ring *r)
{
	WRITE_ONCE(r->ctrl->want_kick.val, 0);
}


u32 mlink_ring_pending(struct mlink_ring *r)
{
	return READ_ONCE(r->ctrl->prod.val) - r->idx;
}
