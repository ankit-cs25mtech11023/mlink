/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * mlink control plane: message format and /dev/mlink_ctrl ioctl (P7).
 * Shared by the driver and the userspace tools (mlinkd, mlinkctl), so it only
 * uses the __u* types from <linux/types.h>.
 *
 * A message is a header followed by TLVs, in the spirit of Qualcomm QMI:
 *   REQ  (AP -> modem)  the kernel on the AP assigns txn_id
 *   RESP (modem -> AP)  carries the txn_id of its REQ; routed to the requester
 *   IND  (modem -> AP)  unsolicited; delivered to every open file
 * All fields are little-endian (both ends are x86 in this project).
 */
#ifndef _MLINK_CTRL_H
#define _MLINK_CTRL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MLINK_CTRL_MSG_MAX	1024	/* = control ring slot size */

enum mlink_ctrl_type {
	MLINK_CTRL_REQ = 0,
	MLINK_CTRL_RESP = 1,
	MLINK_CTRL_IND = 2,
};

enum mlink_ctrl_msg_id {
	MLINK_MSG_GET_LINK_INFO = 0x0001,	/* RESP TLVs: LINK_STATE, MTU, PEER_GEN */
	MLINK_MSG_GET_STATS = 0x0002,		/* RESP TLVs: RX/TX_PKTS, RX/TX_BYTES */
	MLINK_MSG_SET_DATA_CALL = 0x0003,	/* REQ TLV: DATA_CALL (u8 0/1) */
	MLINK_MSG_IND_DATA_CALL = 0x0081,	/* IND TLV: DATA_CALL */
};

enum mlink_ctrl_tlv_type {
	MLINK_TLV_LINK_STATE = 0x01,	/* __u32 */
	MLINK_TLV_MTU = 0x02,		/* __u32 */
	MLINK_TLV_PEER_GEN = 0x03,	/* __u32 */
	MLINK_TLV_RX_PKTS = 0x10,	/* __u64 */
	MLINK_TLV_TX_PKTS = 0x11,	/* __u64 */
	MLINK_TLV_RX_BYTES = 0x12,	/* __u64 */
	MLINK_TLV_TX_BYTES = 0x13,	/* __u64 */
	MLINK_TLV_DATA_CALL = 0x20,	/* __u8: 1 = up */
};

struct mlink_ctrl_hdr {
	__u16 msg_id;
	__u8 type;		/* enum mlink_ctrl_type */
	__u8 rsvd;
	__u32 txn_id;		/* assigned by the AP kernel for REQ, echoed in RESP */
	__u16 len;		/* bytes of TLVs that follow the header */
	__s16 result;		/* RESP: 0 or a negative errno (e.g. -ENOTCONN on SSR) */
};

struct mlink_ctrl_tlv {
	__u8 type;
	__u8 rsvd;
	__u16 len;		/* bytes of value */
	__u8 val[];
};

struct mlink_ctrl_info {
	__u32 role;		/* 0 = ap, 1 = modem */
	__u32 link_state;	/* 0 DOWN, 1 WAIT_PEER, 2 UP */
	__u32 gen;
	__u32 peer_gen;
	__u32 own_id;
	__u32 peer_id;
};

#define MLINK_IOC_GET_INFO	_IOR('M', 1, struct mlink_ctrl_info)

#endif /* _MLINK_CTRL_H */
