// SPDX-License-Identifier: GPL-2.0-only
/*
 * mlinkctl - AP-side control CLI (P7.4).
 *   mlinkctl info            local ioctl info + modem's GET_LINK_INFO
 *   mlinkctl stats           modem's mlink0 counters (GET_STATS)
 *   mlinkctl call up|down    SET_DATA_CALL, then show the indication
 *   mlinkctl monitor         print indications until interrupted
 *   mlinkctl bench N         N GET_LINK_INFO round trips: control-plane latency
 */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "mlink_ctrl_lib.h"

static int fd;

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Wait for the next message of 'type' (others are printed), timeout in ms. */
static ssize_t wait_msg(struct msgbuf *m, uint8_t type, int timeout_ms)
{
	for (;;) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		ssize_t n;

		if (poll(&p, 1, timeout_ms) <= 0) {
			errno = ETIMEDOUT;
			return -1;
		}
		n = read(fd, m->raw, sizeof(m->raw));
		if (n < 0)
			return -1;
		if (m->h.type == type)
			return n;
		if (m->h.type == MLINK_CTRL_IND)
			printf("(indication msg 0x%04x)\n", m->h.msg_id);
	}
}

/* One request/response; returns response length or -1 (errno set). */
static ssize_t transact(uint16_t id, struct msgbuf *req, struct msgbuf *resp)
{
	ssize_t n;

	req->h.msg_id = id;
	req->h.type = MLINK_CTRL_REQ;
	if (write(fd, req->raw, msg_size(req)) < 0)
		return -1;
	n = wait_msg(resp, MLINK_CTRL_RESP, 2000);
	if (n >= 0 && resp->h.result) {
		errno = -resp->h.result;
		return -1;
	}
	return n;
}

static void print_u32(const struct msgbuf *m, size_t n, uint8_t t, const char *name)
{
	const uint32_t *v = tlv_get(m, n, t, 4);

	if (v)
		printf("  %-12s %u\n", name, *v);
}

static void print_u64(const struct msgbuf *m, size_t n, uint8_t t, const char *name)
{
	const uint64_t *v = tlv_get(m, n, t, 8);

	if (v)
		printf("  %-12s %llu\n", name, (unsigned long long)*v);
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
	struct msgbuf req, resp;
	ssize_t n;

	if (argc < 2)
		goto usage;
	fd = open("/dev/mlink_ctrl", O_RDWR);
	if (fd < 0) {
		perror("/dev/mlink_ctrl");
		return 1;
	}
	msg_init(&req, 0, MLINK_CTRL_REQ, 0, 0);

	if (!strcmp(argv[1], "info")) {
		struct mlink_ctrl_info i;

		if (ioctl(fd, MLINK_IOC_GET_INFO, &i) == 0)
			printf("local: role %s link_state %u gen %u peer_gen %u own_id %u peer_id %u\n",
			       i.role ? "modem" : "ap", i.link_state, i.gen, i.peer_gen,
			       i.own_id, i.peer_id);
		n = transact(MLINK_MSG_GET_LINK_INFO, &req, &resp);
		if (n < 0) {
			perror("GET_LINK_INFO");
			return 1;
		}
		printf("modem:\n");
		print_u32(&resp, n, MLINK_TLV_LINK_STATE, "link_state");
		print_u32(&resp, n, MLINK_TLV_PEER_GEN, "peer_gen");
		print_u32(&resp, n, MLINK_TLV_MTU, "mtu");
	} else if (!strcmp(argv[1], "stats")) {
		n = transact(MLINK_MSG_GET_STATS, &req, &resp);
		if (n < 0) {
			perror("GET_STATS");
			return 1;
		}
		printf("modem mlink0:\n");
		print_u64(&resp, n, MLINK_TLV_RX_PKTS, "rx_packets");
		print_u64(&resp, n, MLINK_TLV_TX_PKTS, "tx_packets");
		print_u64(&resp, n, MLINK_TLV_RX_BYTES, "rx_bytes");
		print_u64(&resp, n, MLINK_TLV_TX_BYTES, "tx_bytes");
	} else if (!strcmp(argv[1], "call") && argc > 2) {
		uint8_t up = !strcmp(argv[2], "up");
		const uint8_t *st;

		tlv_put(&req, MLINK_TLV_DATA_CALL, &up, 1);
		n = transact(MLINK_MSG_SET_DATA_CALL, &req, &resp);
		if (n < 0) {
			perror("SET_DATA_CALL");
			return 1;
		}
		st = tlv_get(&resp, n, MLINK_TLV_DATA_CALL, 1);
		printf("data call: %s\n", st && *st ? "up" : "down");
		n = wait_msg(&resp, MLINK_CTRL_IND, 2000);
		st = n > 0 ? tlv_get(&resp, n, MLINK_TLV_DATA_CALL, 1) : NULL;
		printf("indication: data call %s\n", st ? (*st ? "up" : "down") : "(none)");
	} else if (!strcmp(argv[1], "monitor")) {
		for (;;) {
			const uint8_t *st;

			n = wait_msg(&resp, MLINK_CTRL_IND, -1);
			if (n < 0) {
				perror("monitor");
				return 1;
			}
			st = tlv_get(&resp, n, MLINK_TLV_DATA_CALL, 1);
			printf("IND msg 0x%04x data_call %s\n", resp.h.msg_id,
			       st ? (*st ? "up" : "down") : "-");
			fflush(stdout);
		}
	} else if (!strcmp(argv[1], "bench") && argc > 2) {
		long i, cnt = atol(argv[2]);
		uint64_t *rtt = calloc(cnt > 0 ? cnt : 1, sizeof(*rtt));

		if (!rtt || cnt <= 0)
			goto usage;
		for (i = 0; i < cnt; i++) {
			uint64_t t0 = now_ns();

			msg_init(&req, 0, MLINK_CTRL_REQ, 0, 0);
			if (transact(MLINK_MSG_GET_LINK_INFO, &req, &resp) < 0) {
				perror("bench");
				return 1;
			}
			rtt[i] = now_ns() - t0;
		}
		qsort(rtt, cnt, sizeof(*rtt), cmp_u64);
		printf("ctrl GET_LINK_INFO n=%ld min=%.1f p50=%.1f p90=%.1f p99=%.1f max=%.1f us\n",
		       cnt, rtt[0] / 1e3, rtt[cnt / 2] / 1e3, rtt[cnt * 9 / 10] / 1e3,
		       rtt[(cnt - 1) * 99 / 100] / 1e3, rtt[cnt - 1] / 1e3);
	} else {
		goto usage;
	}
	return 0;

usage:
	fprintf(stderr, "usage: mlinkctl info | stats | call up|down | monitor | bench N\n");
	return 2;
}
