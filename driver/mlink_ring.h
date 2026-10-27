/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Single-producer/single-consumer ring over shared memory (design.md §5-6).
 * One side produces into a ring, the other consumes; each side keeps a local
 * copy of the index it owns and only reads the other index from shm.
 */
#ifndef _MLINK_RING_H
#define _MLINK_RING_H

#include <linux/types.h>

#include "mlink_shm.h"

struct mlink_ring {
	struct mlink_ring_ctrl *ctrl;
	struct mlink_desc *desc;
	u8 *slots;
	u32 mask;		/* entries - 1 */
	u32 idx;		/* local copy of the index this side owns */
	u32 seen;		/* last peer index read (diagnostics on -EIO) */
};

/* A validated view of the next descriptor, read from shm exactly once. */
struct mlink_rx_desc {
	u32 len;
	u16 proto;
	u16 flags;
	u32 seq;
	u16 csum_start;
	u16 csum_offset;
	const u8 *data;
};

/* What the producer tells the consumer about one packet. */
struct mlink_tx_meta {
	u16 proto;
	u16 flags;
	u16 csum_start;
	u16 csum_offset;
};

void mlink_ring_attach(struct mlink_ring *r, void *base, bool producer);
void mlink_ring_reset_producer(struct mlink_ring *r);
void mlink_ring_reset_consumer(struct mlink_ring *r);

/* producer side */
int mlink_ring_free(struct mlink_ring *r);
void *mlink_ring_slot(struct mlink_ring *r);
void mlink_ring_publish(struct mlink_ring *r, u32 len, const struct mlink_tx_meta *m, u32 seq);
bool mlink_ring_need_kick(struct mlink_ring *r, bool safe);
bool mlink_ring_arm_space(struct mlink_ring *r, bool safe);
void mlink_ring_disarm_space(struct mlink_ring *r);

/* consumer side */
int mlink_ring_peek(struct mlink_ring *r, struct mlink_rx_desc *d);
void mlink_ring_consume(struct mlink_ring *r);
void mlink_ring_release(struct mlink_ring *r);
bool mlink_ring_need_space_kick(struct mlink_ring *r, bool safe);
bool mlink_ring_arm_kick(struct mlink_ring *r, bool safe);
void mlink_ring_disarm_kick(struct mlink_ring *r);
u32 mlink_ring_pending(struct mlink_ring *r);

#endif /* _MLINK_RING_H */
