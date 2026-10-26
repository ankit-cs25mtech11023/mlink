// SPDX-License-Identifier: GPL-2.0-only
/*
 * udp_pingpong - UDP request/response round-trip latency (P6.3).
 *
 *   server:  udp_pingpong -s [-p port] [-b busy_poll_us]
 *   client:  udp_pingpong -c HOST [-p port] [-n count] [-l bytes] [-i interval_us]
 *                         [-w warmup] [-b busy_poll_us] [-o samples.csv]
 *
 * One message in flight at a time; RTT = CLOCK_MONOTONIC delta on the client
 * (one clock, so no cross-VM clock sync problem). Percentiles come from the
 * sorted samples (exact); -o writes every sample for plotting. -b enables
 * SO_BUSY_POLL on the socket (needs net.core.busy_read or this per-socket
 * option, and a NAPI-driven device: mlink0 is).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46
#endif

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static uint64_t pct(const uint64_t *v, size_t n, double p)
{
	size_t i = (size_t)((double)(n - 1) * p / 100.0);

	return v[i];
}

static void set_busy_poll(int fd, int us)
{
	if (us > 0 && setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &us, sizeof(us)) < 0)
		perror("SO_BUSY_POLL (needs CAP_NET_ADMIN to raise above net.core.busy_read)");
}

static int server(int port, int busy)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
	char buf[65536];
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		perror("server socket/bind");
		return 1;
	}
	set_busy_poll(fd, busy);
	fprintf(stderr, "udp_pingpong server on :%d (busy_poll %d us)\n", port, busy);
	for (;;) {
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("recvfrom");
			return 1;
		}
		sendto(fd, buf, n, 0, (struct sockaddr *)&from, fl);
	}
}

static int client(const char *host, int port, long count, int len, long interval_us,
		  long warmup, int busy, const char *csv)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
	struct timeval tmo = { .tv_sec = 1 };
	uint64_t *rtt = calloc(count, sizeof(*rtt));
	char *buf = calloc(1, len > 8 ? len : 8);
	long i, got = 0, lost = 0;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	FILE *out = NULL;

	if (!rtt || !buf || fd < 0 || inet_pton(AF_INET, host, &a.sin_addr) != 1) {
		fprintf(stderr, "client setup failed (host must be an IPv4 address)\n");
		return 1;
	}
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		perror("connect");
		return 1;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
	set_busy_poll(fd, busy);

	for (i = -warmup; i < count; i++) {
		uint64_t t0, t1, seq = (uint64_t)i;
		ssize_t n;

		memcpy(buf, &seq, sizeof(seq));
		t0 = now_ns();
		if (send(fd, buf, len, 0) != len) {
			perror("send");
			return 1;
		}
		do {	/* skip stale replies to earlier timed-out requests */
			n = recv(fd, buf, len, 0);
		} while (n >= 8 && memcmp(buf, &seq, sizeof(seq)) != 0);
		t1 = now_ns();
		if (n < 0) {
			if (i >= 0)
				lost++;
			continue;
		}
		if (i >= 0)
			rtt[got++] = t1 - t0;
		if (interval_us > 0) {
			struct timespec ts = { interval_us / 1000000, (interval_us % 1000000) * 1000 };

			nanosleep(&ts, NULL);
		}
	}

	if (csv && (out = fopen(csv, "w"))) {
		fprintf(out, "sample,rtt_ns\n");
		for (i = 0; i < got; i++)
			fprintf(out, "%ld,%llu\n", i, (unsigned long long)rtt[i]);
		fclose(out);
	}
	if (!got) {
		printf("no replies (lost %ld)\n", lost);
		return 1;
	}
	qsort(rtt, got, sizeof(*rtt), cmp_u64);
	/* One machine-readable line; units are microseconds. */
	printf("udp_pingpong len=%d n=%ld lost=%ld busy_poll=%d min=%.1f p50=%.1f p90=%.1f p99=%.1f p999=%.1f max=%.1f us\n",
	       len, got, lost, busy, rtt[0] / 1e3, pct(rtt, got, 50) / 1e3,
	       pct(rtt, got, 90) / 1e3, pct(rtt, got, 99) / 1e3, pct(rtt, got, 99.9) / 1e3,
	       rtt[got - 1] / 1e3);
	return 0;
}

int main(int argc, char **argv)
{
	const char *host = NULL, *csv = NULL;
	long count = 10000, interval = 0, warmup = 100;
	int port = 7777, len = 64, busy = 0, srv = 0, c;

	while ((c = getopt(argc, argv, "sc:p:n:l:i:w:b:o:")) != -1) {
		switch (c) {
		case 's': srv = 1; break;
		case 'c': host = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 'n': count = atol(optarg); break;
		case 'l': len = atoi(optarg); break;
		case 'i': interval = atol(optarg); break;
		case 'w': warmup = atol(optarg); break;
		case 'b': busy = atoi(optarg); break;
		case 'o': csv = optarg; break;
		default: goto usage;
		}
	}
	if (srv)
		return server(port, busy);
	if (!host || count <= 0 || len < 8 || len > 65000)
		goto usage;
	return client(host, port, count, len, interval, warmup, busy, csv);

usage:
	fprintf(stderr, "usage: %s -s [-p port] [-b busy_us]\n"
			"       %s -c HOST [-p port] [-n count] [-l bytes>=8] [-i interval_us] [-w warmup] [-b busy_us] [-o csv]\n",
		argv[0], argv[0]);
	return 2;
}
