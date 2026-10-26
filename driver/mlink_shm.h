/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mlink shared-memory ABI. Both guests map this layout from ivshmem BAR2.
 *
 * Rules (see docs/design.md):
 *  - Every field has exactly ONE writer side, named in the comment next to it.
 *  - Everything here is read from untrusted memory: read once, then validate.
 *  - Both sides are x86 (little-endian) in the MVP, so fields are plain u32.
 *    A big-endian or mixed peer would need __le32 + le32_to_cpu() (P9 note).
 */
#ifndef _MLINK_SHM_H
#define _MLINK_SHM_H

#include <linux/build_bug.h>
#include <linux/stddef.h>
#include <linux/types.h>

#define MLINK_SHM_MAGIC		0x4b4e4c4d	/* "MLNK" in memory order */
#define MLINK_STATE_MAGIC	0x54534c4d	/* "MLST" */
#define MLINK_ABI_VERSION	1

/* Fixed layout (MVP). */
#define MLINK_RING_ENTRIES	256		/* power of two */
#define MLINK_SLOT_SIZE		4096
#define MLINK_HDR_OFF		0x000000
#define MLINK_STATE_AP_OFF	0x001000
#define MLINK_STATE_MODEM_OFF	0x001040
#define MLINK_RING_A_OFF	0x002000	/* AP -> MODEM */
#define MLINK_RING_B_OFF	0x104000	/* MODEM -> AP */
#define MLINK_LAYOUT_END	0x206000	/* BAR2 must be at least this big */

/* Interrupt vectors: the meaning of a kick on vector N, see design.md §7. */
enum mlink_vector {
	MLINK_VEC_DATA,
	MLINK_VEC_SPACE,
	MLINK_VEC_STATE,
	MLINK_VEC_TEST,
	MLINK_NR_VECTORS,
};

/* Global header at MLINK_HDR_OFF. Writer: AP, once, magic last. */
struct mlink_shm_hdr {
	u32 magic;		/* MLINK_SHM_MAGIC, published with release */
	u32 abi_version;
	u32 ring_entries;
	u32 slot_size;
	u32 ring_a_off;
	u32 ring_b_off;
	u32 layout_crc;		/* crc32_le over abi_version..ring_b_off */
	u32 rsvd[9];
};
static_assert(sizeof(struct mlink_shm_hdr) == 64);
static_assert(offsetof(struct mlink_shm_hdr, abi_version) == 4);
static_assert(offsetof(struct mlink_shm_hdr, ring_b_off) == 20);
static_assert(offsetof(struct mlink_shm_hdr, layout_crc) == 24);

/* The fields covered by layout_crc. */
#define MLINK_HDR_CRC_START	offsetof(struct mlink_shm_hdr, abi_version)
#define MLINK_HDR_CRC_LEN	(offsetof(struct mlink_shm_hdr, layout_crc) - \
				 MLINK_HDR_CRC_START)

/* Link states published in mlink_shm_state.state. */
enum mlink_link_state {
	MLINK_ST_DOWN,
	MLINK_ST_WAIT_PEER,
	MLINK_ST_UP,
};

/*
 * Per-side state block, one 64-byte cache line each so the two writers never
 * share a line. Writer: the owning side only (AP block / MODEM block).
 */
struct mlink_shm_state {
	u32 magic;		/* MLINK_STATE_MAGIC when valid, published with release */
	u32 peer_id;		/* owner's IVPosition (ivshmem peer ID) */
	u32 state;		/* enum mlink_link_state */
	u32 gen;		/* bumped on every (re)start of the owner */
	u32 peer_gen_seen;	/* last peer gen the owner acknowledged (P4) */
	u32 heartbeat;		/* incremented every hb_ms (P4) */
	u32 mtu;
	u32 features;
	u32 rsvd[8];
} __aligned(64);
static_assert(sizeof(struct mlink_shm_state) == 64);
static_assert(offsetof(struct mlink_shm_state, peer_id) == 4);
static_assert(offsetof(struct mlink_shm_state, gen) == 12);
static_assert(offsetof(struct mlink_shm_state, heartbeat) == 20);
static_assert(MLINK_STATE_MODEM_OFF - MLINK_STATE_AP_OFF == sizeof(struct mlink_shm_state));

/* Layout sanity: regions are ordered, aligned and fit in 4 MiB. */
static_assert((MLINK_RING_ENTRIES & (MLINK_RING_ENTRIES - 1)) == 0);
static_assert(MLINK_STATE_AP_OFF >= MLINK_HDR_OFF + sizeof(struct mlink_shm_hdr));
static_assert(MLINK_RING_A_OFF >= MLINK_STATE_MODEM_OFF + sizeof(struct mlink_shm_state));
static_assert(MLINK_LAYOUT_END <= (4 << 20));

#endif /* _MLINK_SHM_H */
