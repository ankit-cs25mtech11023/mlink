// SPDX-License-Identifier: GPL-2.0-only
/*
 * Control plane on the Zephyr modem: what tools/mlinkd does on the Linux
 * modem, but straight on control rings C (requests in) and D (responses and
 * indications out), with no /dev/mlink_ctrl in between. Same message format
 * (driver/mlink_ctrl.h) and the same TLV helpers (tools/mlink_ctrl_lib.h).
 *
 * Framing as in driver/mlink_ctrl.c: one message per slot, proto
 * MLINK_PROTO_CTRL, desc seq = txn_id; the control rings use no kick
 * suppression, every publish or consume rings the CTRL vector.
 * Runs in the poll thread, which is the only producer of ring D.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "modem.h"
#include "mlink_ctrl_lib.h"

static struct msgbuf req, resp;	/* poll thread only */

/* Queue one message on ring D; false (and counted) if the ring is full. */
static bool ctrl_send(const struct msgbuf *msg)
{
	const struct mlink_tx_meta meta = { .proto = MLINK_PROTO_CTRL };
	int n = mlink_ring_free(&m.ctrl_tx);

	if (n < 0) {
		m.tx_broken = true;	/* the link tick resets */
		return false;
	}
	if (n == 0) {
		/* Low rate, AP drains on every CTRL kick: drop instead of blocking. */
		m.st.ctrl_full++;
		return false;
	}
	memcpy(mlink_ring_slot(&m.ctrl_tx), msg->raw, msg_size(msg));
	mlink_ring_publish(&m.ctrl_tx, msg_size(msg), &meta, msg->h.txn_id);
	return true;
}

static bool serve(size_t len)
{
	bool sent;

	msg_init(&resp, req.h.msg_id, MLINK_CTRL_RESP, req.h.txn_id, 0);
	switch (req.h.msg_id) {
	case MLINK_MSG_GET_LINK_INFO: {
		u32 state = m.state, peer_gen = m.peer_gen, mtu = OWN_MTU;

		tlv_put(&resp, MLINK_TLV_LINK_STATE, &state, 4);
		tlv_put(&resp, MLINK_TLV_PEER_GEN, &peer_gen, 4);
		tlv_put(&resp, MLINK_TLV_MTU, &mtu, 4);
		break;
	}
	case MLINK_MSG_GET_STATS: {
		u64 rx = m.st.rx, tx = m.st.tx;

		tlv_put(&resp, MLINK_TLV_RX_PKTS, &rx, 8);
		tlv_put(&resp, MLINK_TLV_TX_PKTS, &tx, 8);
		tlv_put(&resp, MLINK_TLV_RX_BYTES, &m.st.rx_bytes, 8);
		tlv_put(&resp, MLINK_TLV_TX_BYTES, &m.st.tx_bytes, 8);
		break;
	}
	case MLINK_MSG_SET_DATA_CALL: {
		const u8 *want = tlv_get(&req, len, MLINK_TLV_DATA_CALL, 1);
		struct msgbuf ind;
		u8 now;

		if (!want) {
			resp.h.result = -EINVAL;
			break;
		}
		WRITE_ONCE(m.data_call, *want != 0);
		modem_net_carrier(m.link_up && m.data_call);
		now = m.data_call;
		tlv_put(&resp, MLINK_TLV_DATA_CALL, &now, 1);
		sent = ctrl_send(&resp);
		/* Unsolicited: every AP client learns the new state (as mlinkd does). */
		msg_init(&ind, MLINK_MSG_IND_DATA_CALL, MLINK_CTRL_IND, 0, 0);
		tlv_put(&ind, MLINK_TLV_DATA_CALL, &now, 1);
		ctrl_send(&ind);
		printk("mlink-modem: data call -> %s\n", now ? "up" : "down");
		return sent;
	}
	default:
		resp.h.result = -EOPNOTSUPP;
	}
	return ctrl_send(&resp);
}

void modem_ctrl_poll(void)
{
	struct mlink_rx_desc d;
	bool consumed = false, published = false;
	int ret;

	while ((ret = mlink_ring_peek(&m.ctrl_rx, &d)) != 0) {
		if (ret == -EIO) {
			m.rx_broken = true;	/* the link tick resets */
			break;
		}
		if (ret > 0 && d.proto == MLINK_PROTO_CTRL && d.len >= sizeof(req.h)) {
			/* Copy first, then validate only our copy (no double fetch). */
			memcpy(req.raw, d.data, d.len);
			if (sizeof(req.h) + req.h.len == d.len && req.h.type == MLINK_CTRL_REQ) {
				m.st.ctrl_req++;
				if (serve(d.len)) {
					m.st.ctrl_resp++;
					published = true;
				}
			} else {
				m.st.ctrl_invalid++;
			}
		} else {
			m.st.ctrl_invalid++;
		}
		mlink_ring_consume(&m.ctrl_rx);
		consumed = true;
	}
	if (consumed)
		mlink_ring_release(&m.ctrl_rx);
	/* Low rate: always kick, so a writer waiting for space on the AP wakes up. */
	if (consumed || published)
		modem_kick(MLINK_VEC_CTRL);
}
