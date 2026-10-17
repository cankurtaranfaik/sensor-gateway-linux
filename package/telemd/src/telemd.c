// SPDX-License-Identifier: MIT
/*
 * telemd - read IMU samples from /dev/vimu and stream them as UDP frames.
 *
 * Frame layout (little endian, 38 bytes):
 *   u16 magic 0x5347 ("SG")   u8 version   u8 flags
 *   u32 seq                    u64 t_sample_ns   u64 t_send_ns
 *   s16 accel_mg[3]            s16 gyro_mdps[3]
 *   u16 crc  (CRC-16/CCITT-FALSE over the first 36 bytes)
 *
 * Both timestamps are the target's CLOCK_MONOTONIC, so t_send - t_sample is
 * the time a sample spends between the driver and the socket.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "vimu_uapi.h"

#define FRAME_MAGIC	0x5347
#define FRAME_VERSION	1
#define FRAME_LEN	38
#define READ_BATCH	16

struct opts {
	const char *dev;
	const char *host;
	int port;
	int stats_sec;
};

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static uint16_t crc16_ccitt(const uint8_t *p, size_t len)
{
	uint16_t crc = 0xffff;

	while (len--) {
		crc ^= (uint16_t)*p++ << 8;
		for (int i = 0; i < 8; i++)
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
	}
	return crc;
}

static void put16(uint8_t **p, uint16_t v) { v = htole16(v); memcpy(*p, &v, 2); *p += 2; }
static void put32(uint8_t **p, uint32_t v) { v = htole32(v); memcpy(*p, &v, 4); *p += 4; }
static void put64(uint8_t **p, uint64_t v) { v = htole64(v); memcpy(*p, &v, 8); *p += 8; }

static void build_frame(uint8_t *buf, const struct vimu_sample *s, uint64_t t_send)
{
	uint8_t *p = buf;

	put16(&p, FRAME_MAGIC);
	*p++ = FRAME_VERSION;
	*p++ = 0;
	put32(&p, s->seq);
	put64(&p, s->timestamp_ns);
	put64(&p, t_send);
	for (int i = 0; i < 3; i++)
		put16(&p, (uint16_t)s->accel_mg[i]);
	for (int i = 0; i < 3; i++)
		put16(&p, (uint16_t)s->gyro_mdps[i]);
	put16(&p, crc16_ccitt(buf, FRAME_LEN - 2));
}

/* write a line into the kernel log so it lands in the boot log with a timestamp */
static void kmsg(const char *msg)
{
	int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	if (write(fd, msg, strlen(msg)) < 0)
		perror("kmsg");
	close(fd);
}

static unsigned int read_dropped(void)
{
	FILE *f = fopen("/sys/class/misc/vimu/dropped", "r");
	unsigned int v = 0;

	if (f) {
		if (fscanf(f, "%u", &v) != 1)
			v = 0;
		fclose(f);
	}
	return v;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-D device] [-d host] [-p port] [-s stats_sec]\n"
		"defaults: -D /dev/vimu -d 10.0.2.2 -p 5005 -s 5\n", prog);
}

static int parse_opts(int argc, char **argv, struct opts *o)
{
	int c;

	*o = (struct opts){ "/dev/vimu", "10.0.2.2", 5005, 5 };
	while ((c = getopt(argc, argv, "D:d:p:s:h")) != -1) {
		switch (c) {
		case 'D': o->dev = optarg; break;
		case 'd': o->host = optarg; break;
		case 'p': o->port = atoi(optarg); break;
		case 's': o->stats_sec = atoi(optarg); break;
		default: usage(argv[0]); return -1;
		}
	}
	if (o->port <= 0 || o->port > 65535 || o->stats_sec <= 0) {
		usage(argv[0]);
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct vimu_sample batch[READ_BATCH];
	uint8_t frame[FRAME_LEN];
	struct sockaddr_in dst = { .sin_family = AF_INET };
	struct sigaction sa = { .sa_handler = on_signal };
	unsigned long long sent = 0, send_err = 0;
	uint64_t next_stats;
	int first = 1;
	struct opts o;
	int fd, sock;

	if (parse_opts(argc, argv, &o))
		return 2;

	dst.sin_port = htons(o.port);
	if (inet_pton(AF_INET, o.host, &dst.sin_addr) != 1) {
		fprintf(stderr, "telemd: bad address %s\n", o.host);
		return 2;
	}

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	fd = open(o.dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "telemd: open %s: %s\n", o.dev, strerror(errno));
		return 1;
	}

	sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (sock < 0) {
		perror("telemd: socket");
		return 1;
	}

	fprintf(stderr, "telemd: %s -> %s:%d\n", o.dev, o.host, o.port);
	next_stats = now_ns() + (uint64_t)o.stats_sec * 1000000000ull;

	while (!stop) {
		ssize_t n = read(fd, batch, sizeof(batch));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("telemd: read");
			break;
		}

		for (size_t i = 0; i < (size_t)n / sizeof(batch[0]); i++) {
			build_frame(frame, &batch[i], now_ns());
			if (sendto(sock, frame, sizeof(frame), 0,
				   (struct sockaddr *)&dst, sizeof(dst)) < 0) {
				send_err++;
				continue;
			}
			sent++;
			if (first) {
				kmsg("telemd: first frame sent\n");
				first = 0;
			}
		}

		if (now_ns() >= next_stats) {
			fprintf(stderr, "telemd: sent=%llu send_err=%llu dropped=%u\n",
				sent, send_err, read_dropped());
			next_stats += (uint64_t)o.stats_sec * 1000000000ull;
		}
	}

	fprintf(stderr, "telemd: exiting, sent=%llu\n", sent);
	close(sock);
	close(fd);
	return 0;
}
