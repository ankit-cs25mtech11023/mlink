// SPDX-License-Identifier: GPL-2.0-only
/*
 * shm_chaos - corrupt the mlink shared memory from the HOST while both guests
 * run, to prove the driver treats shared memory as untrusted input (P5.1).
 *
 * It maps the ivshmem POSIX shm object (/dev/shm/mlink-shm) and, at a fixed
 * rate, applies one kind of damage. The layout constants come from the
 * driver's ABI header so the tool and the driver can never disagree.
 *
 * Usage: shm_chaos -m MODE [-r ops_per_s] [-d seconds] [-s seed] [-f file]
 *   MODE: flip   random bit flips in descriptors and ring control words
 *         len    bogus descriptor lengths (0, > slot size, huge)
 *         idx    jump a producer or consumer index (prod += 1000 etc.)
 *         flags  storm of want_kick / want_space toggles
 *         magic  corrupt the header or a state-block magic
 *         hb     freeze one side's heartbeat (rewrite it with a constant)
 *         mix    a random mode per operation
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../driver/mlink_shm.h"	/* the driver's ABI, userspace-safe */

static volatile u32 *u32_at(uint8_t *shm, size_t off)
{
	return (volatile u32 *)(shm + off);
}

static uint64_t rng_state;

static uint32_t rnd(void)
{
	/* xorshift64*: deterministic for a given -s seed */
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return (uint32_t)((rng_state * 2685821657736338717ULL) >> 32);
}

static size_t ring_off(int which)
{
	return which ? MLINK_RING_B_OFF : MLINK_RING_A_OFF;
}

static const char *ring_name(int which)
{
	return which ? "B(modem->ap)" : "A(ap->modem)";
}

static void op_flip(uint8_t *shm)
{
	int ring = rnd() & 1;
	size_t base = ring_off(ring);
	size_t off;

	if (rnd() & 1)	/* a descriptor byte */
		off = base + MLINK_RING_DESC_OFF + rnd() % (MLINK_RING_ENTRIES * sizeof(struct mlink_desc));
	else		/* one of the four control words */
		off = base + (rnd() % 4) * 64 + (rnd() % 4);
	shm[off] ^= (uint8_t)(1u << (rnd() % 8));
	printf("flip  ring %s off 0x%zx\n", ring_name(ring), off);
}

static void op_len(uint8_t *shm)
{
	static const u32 bad[] = { 0, MLINK_SLOT_SIZE + 1, 0x7fffffff, 0xffffffff };
	int ring = rnd() & 1;
	u32 i = rnd() % MLINK_RING_ENTRIES;
	u32 v = bad[rnd() % 4];
	size_t off = ring_off(ring) + MLINK_RING_DESC_OFF + i * sizeof(struct mlink_desc);

	*u32_at(shm, off + offsetof(struct mlink_desc, len)) = v;
	printf("len   ring %s desc %u len=0x%x\n", ring_name(ring), i, v);
}

static void op_idx(uint8_t *shm)
{
	int ring = rnd() & 1;
	int cons = rnd() & 1;
	size_t off = ring_off(ring) + (cons ? offsetof(struct mlink_ring_ctrl, cons) :
					       offsetof(struct mlink_ring_ctrl, prod));
	u32 jump = (rnd() & 1) ? 1000 : (u32)-(int)(MLINK_RING_ENTRIES * 2);

	*u32_at(shm, off) += jump;
	printf("idx   ring %s %s += %d\n", ring_name(ring), cons ? "cons" : "prod", (int)jump);
}

static void op_flags(uint8_t *shm)
{
	int ring = rnd() & 1;
	size_t base = ring_off(ring);
	int i;

	for (i = 0; i < 64; i++) {
		*u32_at(shm, base + offsetof(struct mlink_ring_ctrl, want_kick)) = rnd() & 1;
		*u32_at(shm, base + offsetof(struct mlink_ring_ctrl, want_space)) = rnd() & 1;
	}
	printf("flags ring %s 64 random toggles\n", ring_name(ring));
}

static void op_magic(uint8_t *shm)
{
	static const size_t offs[] = {
		MLINK_HDR_OFF, MLINK_STATE_AP_OFF, MLINK_STATE_MODEM_OFF,
	};
	static const char * const names[] = { "header", "state AP", "state MODEM" };
	int i = rnd() % 3;

	*u32_at(shm, offs[i]) ^= 0xdeadbeef;
	printf("magic %s\n", names[i]);
}

static void op_hb(uint8_t *shm, double seconds)
{
	size_t off = ((rnd() & 1) ? MLINK_STATE_MODEM_OFF : MLINK_STATE_AP_OFF) +
		     offsetof(struct mlink_shm_state, heartbeat);
	u32 frozen = *u32_at(shm, off);
	struct timespec ts = { 0, 1000000 };	/* 1 ms */
	long n = (long)(seconds * 1000);

	printf("hb    freeze %s heartbeat at %u for %.1f s\n",
	       off < MLINK_STATE_MODEM_OFF ? "AP" : "MODEM", frozen, seconds);
	fflush(stdout);
	while (n--) {
		*u32_at(shm, off) = frozen;
		nanosleep(&ts, NULL);
	}
}

int main(int argc, char **argv)
{
	const char *file = "/dev/shm/mlink-shm", *mode = NULL;
	double rate = 10, duration = 10;
	unsigned long seed = (unsigned long)time(NULL);
	struct timespec gap;
	uint8_t *shm;
	struct stat st;
	long ops, done;
	int fd, c;

	while ((c = getopt(argc, argv, "m:r:d:s:f:")) != -1) {
		switch (c) {
		case 'm': mode = optarg; break;
		case 'r': rate = atof(optarg); break;
		case 'd': duration = atof(optarg); break;
		case 's': seed = strtoul(optarg, NULL, 0); break;
		case 'f': file = optarg; break;
		default: goto usage;
		}
	}
	if (!mode || rate <= 0 || rate > 10000 || duration <= 0)
		goto usage;

	fd = open(file, O_RDWR);
	if (fd < 0 || fstat(fd, &st) < 0) {
		perror(file);
		return 1;
	}
	if ((size_t)st.st_size < MLINK_LAYOUT_END) {
		fprintf(stderr, "%s: too small (%lld bytes)\n", file, (long long)st.st_size);
		return 1;
	}
	shm = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shm == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	rng_state = seed ? seed : 1;
	printf("shm_chaos: mode=%s rate=%.1f/s duration=%.1fs seed=%lu file=%s\n",
	       mode, rate, duration, seed, file);
	if (!strcmp(mode, "hb")) {
		op_hb(shm, duration);
		return 0;
	}

	ops = (long)(rate * duration);
	gap.tv_sec = (time_t)(1.0 / rate);
	gap.tv_nsec = (long)((1.0 / rate - (double)gap.tv_sec) * 1e9);
	for (done = 0; done < ops; done++) {
		const char *m = mode;

		if (!strcmp(mode, "mix")) {
			static const char * const all[] = { "flip", "len", "idx", "flags", "magic" };

			m = all[rnd() % 5];
		}
		if (!strcmp(m, "flip"))
			op_flip(shm);
		else if (!strcmp(m, "len"))
			op_len(shm);
		else if (!strcmp(m, "idx"))
			op_idx(shm);
		else if (!strcmp(m, "flags"))
			op_flags(shm);
		else if (!strcmp(m, "magic"))
			op_magic(shm);
		else
			goto usage;
		fflush(stdout);
		nanosleep(&gap, NULL);
	}
	printf("shm_chaos: %ld operations\n", done);
	return 0;

usage:
	fprintf(stderr, "usage: %s -m flip|len|idx|flags|magic|hb|mix [-r ops/s] [-d s] [-s seed] [-f file]\n",
		argv[0]);
	return 2;
}
