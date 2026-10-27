/* SPDX-License-Identifier: GPL-2.0-only */
/* Small helpers shared by mlinkd and mlinkctl: build and parse TLV messages. */
#ifndef _MLINK_CTRL_LIB_H
#define _MLINK_CTRL_LIB_H

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "../driver/mlink_ctrl.h"

struct msgbuf {
	union {
		struct mlink_ctrl_hdr h;
		uint8_t raw[MLINK_CTRL_MSG_MAX];
	};
};

static inline void msg_init(struct msgbuf *m, uint16_t id, uint8_t type, uint32_t txn, int16_t res)
{
	memset(m, 0, sizeof(*m));
	m->h.msg_id = id;
	m->h.type = type;
	m->h.txn_id = txn;
	m->h.result = res;
}

static inline size_t msg_size(const struct msgbuf *m)
{
	return sizeof(m->h) + m->h.len;
}

/* Append one TLV; returns 0 or -ENOSPC. */
static inline int tlv_put(struct msgbuf *m, uint8_t type, const void *val, uint16_t len)
{
	size_t off = sizeof(m->h) + m->h.len;
	struct mlink_ctrl_tlv *t = (void *)(m->raw + off);

	if (off + sizeof(*t) + len > sizeof(m->raw))
		return -ENOSPC;
	t->type = type;
	t->rsvd = 0;
	t->len = len;
	memcpy(t->val, val, len);
	m->h.len += sizeof(*t) + len;
	return 0;
}

/* Find a TLV in a received message (bounds-checked); NULL if absent/short. */
static inline const void *tlv_get(const struct msgbuf *m, size_t msglen, uint8_t type,
				  uint16_t want_len)
{
	size_t off = sizeof(m->h);

	while (off + sizeof(struct mlink_ctrl_tlv) <= msglen) {
		const struct mlink_ctrl_tlv *t = (const void *)(m->raw + off);

		if (off + sizeof(*t) + t->len > msglen)
			return NULL;
		if (t->type == type)
			return t->len >= want_len ? t->val : NULL;
		off += sizeof(*t) + t->len;
	}
	return NULL;
}

#endif
