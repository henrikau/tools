/*
 * Copyright (c)  2021 SINTEF
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <argp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <pthread.h>

#include <stdbool.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <arpa/inet.h>

static struct argp_option options[] = {
	{"throttle", 't', NULL , 0, "Vary the used BW from the sender. Goes betwen 0 and 100% in 10% increments every PERIOD seconds" },
	{"period", 'p', "PERIOD", 0, "Send data peridically, i.e. [PERIOD] on, [PERIOD] off (in seconds)"},
	{"recv"   , 'r', 0        , 0, "Act as receiver" },
	{"size"   , 's', "BYTES"  , 0, "Bytes to send in each frame (1500 max)"},
	{"ipv4"   , 'I', "IPv4"   , 0, "Target address for data" },
	{ 0 }
};

static char ipv4[16] = {0};
static bool receiver = false;
static int sz = 1500;
static bool throttle = false;
static int period = -1;

/*
 * CRC: 4
 * UDP: 8
 * IPv4: 20
 * Mac: 14
 * Preamble: 7+1
 * IPG: 12
 */
const int hdr_size = 8 + 14 + 20 + 8 + 4 + 12;

error_t parser(int key, char *arg, struct argp_state *state)
{
	int tmp = 0;
	switch (key) {
	case 'I':
		strncpy(ipv4, arg, sizeof(ipv4) - 1);
		break;
	case 'p':
		period = atoi(arg);
		if (period < 0 || period > 3600)
			period = 0;
		break;
	case 'r':
		receiver = true;
		break;
	case 's':
		tmp = atoi(arg);
		if (tmp < 46) {
			printf("Minimum size is 46, %d is too small, adjusting...\n", tmp);
			sz = 46;
		} else if (tmp > 1500) {
			printf("Maximum payload size is 1500 octets (%d too large), adjusting...\n", tmp);
			sz = 1500;
		} else {
			sz = tmp;
		}
		break;
	case 't':
		throttle = true;
		break;
	}
	return 0;
}

static bool running = false;
static int sockfd = -1;
void signal_handler(int signal)
{
	(void)signal;
	running = false;
	if (sockfd > 0 && receiver)
		shutdown(sockfd, SHUT_RDWR);

	/* get rid of ^C at start of next line printed, annoying */
	printf("\n");
	fflush(stdout);
}

int create_socket(int timeout_us)
{
	int fd  = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd == -1){
		perror("Failed creating UDP socket");
		return -1;
	}
	if (timeout_us > 0) {
		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = timeout_us;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
	}
	int sockprio = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &sockprio, sizeof(sockprio)) < 0) {
		fprintf(stderr, "%s(): failed setting socket priority (%d, %s)\n",
			__func__, errno, strerror(errno));
	}

	return fd;
}

struct hdr {
	int32_t seqnr;
} __attribute__((packed));
int frames_seen;
int max_frames_seen = -1;

void* bw_monitor(void *n)
{
	/* Get basetime and assuming clock is synchronized properly..  */
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);

	int frames_last = frames_seen;
	int payload_size = *(int *)n + hdr_size;
	int max_frames = 1e9 / (payload_size*8);

	while (running) {
		ts.tv_sec += 1;
		ts.tv_nsec = 0;
		if (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, NULL) == -1) {
			printf("%s(): clock_nanosleep failed (%d, %s), stopping receiver\n",
				__func__, errno, strerror(errno));
			running = false;
		}

		int diff = frames_seen - frames_last;
		if (diff > max_frames_seen)
			max_frames_seen = diff;

		time_t timer;
		char buffer[26];
		struct tm* tm_info;
		timer = time(NULL);
		tm_info = localtime(&timer);
		strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

		/* if period > 0, update sleep-length provided period has passed since last time */
		printf("%s current bw: %8.3f Mbps (%d pkts/sec - %.3f %%) (current max: %d of %d\n",
			buffer,
			((double)diff * payload_size)/1e6*8,
			diff,
			(double)diff * 100.0/max_frames,
			max_frames_seen,
			max_frames);
		frames_last += diff;
	}
	return NULL;
}

int rx_loop(int sock, int payload_size)
{

	struct sockaddr_in sin = { 0 };
	sin.sin_family = AF_INET;
	sin.sin_port = htons(4242);
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(sock, (struct sockaddr *)&sin, sizeof(sin)) == -1) {
		fprintf(stderr, "bind() failed\n");
		return -1;
	}

	struct sockaddr_in si_other;
	unsigned int slen = sizeof(si_other);

	unsigned char buffer[1500];
	struct hdr *hdr = (struct hdr *)&buffer[0];

	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	running = true;
	signal(SIGINT, signal_handler);
	pthread_t bwtid;
	pthread_create(&bwtid, NULL, bw_monitor, (int *)&payload_size);

	while (running) {
		int recv_len = recvfrom(sockfd, (void *)buffer, 1500, 0, (struct sockaddr *)&si_other, &slen);
		if (recv_len == -1)
			continue;

		if (hdr->seqnr == -1) {
			printf("Terminator received, closing\n");
			running = false;
			continue;
		}
		if (recv_len != payload_size) {
			fprintf(stderr, "invalid length received, expected %d, got %d\n",
				payload_size, recv_len);
			continue;
		}
		frames_seen++;
	}

	clock_gettime(CLOCK_REALTIME, &end);
	pthread_join(bwtid, NULL);

	double data_sent = frames_seen * (payload_size + 8 + 18 + 4); /* udp + ether + crc */
	uint64_t dur_ns = (end.tv_sec - start.tv_sec)*1e9 + (end.tv_nsec - start.tv_nsec);
	double bw_us = data_sent * 8 / (dur_ns / 1000);
	/* udp header: 8 bytes
	 * etherheader: 18 + 4 crc
	 */
	printf("Ran for %.3f sec\n", (double)dur_ns / 1e9);
	printf("Received %d packets\n", frames_seen);
	printf("Packet size: %d bytes (excl. header)\n", payload_size);
	printf("Avg BW: %.3f Mbps\n", bw_us);
	printf("Max frames/sec seen: %d\n", max_frames_seen);
	double link_util = ((payload_size + 8 + 18 + 4 + 8) * 8 + 96) * max_frames_seen / 1e7;
	printf("Max link util: %.3f%%\n", link_util);
	return 0;
}

int tx_loop(int sock, int payload_size)
{
	int frames_sent = 0;
	struct sockaddr_in sin = { 0 };
	sin.sin_family = AF_INET;
	sin.sin_port = htons(4242);

	if (inet_aton(ipv4, &sin.sin_addr) != 1) {
		fprintf(stderr, "Failed converting server address (%s)\n", ipv4);
		return -1;
	}
	unsigned char *buffer = calloc(1, payload_size);
	if (!buffer)
		return -1;
	struct hdr *hdr = (struct hdr *)&buffer[0];

	running = true;
	signal(SIGINT, signal_handler);

	memset(buffer, 0, payload_size);
	hdr->seqnr = 0xdeadbeef;

	struct timespec ts_now;
	struct timespec ts_period;

	clock_gettime(CLOCK_MONOTONIC, &ts_period);
	ts_period.tv_sec += period;

	while (running) {
		ssize_t sz = sendto(sock, buffer,
				payload_size,
				0,
				(struct sockaddr *)&sin,
				sizeof(struct sockaddr_in));
		if (sz == -1) {
			printf("Failed sending to remote, %s\n", strerror(errno));
			running = false;
			continue;
		}
		clock_gettime(CLOCK_MONOTONIC, &ts_now);
		uint64_t ts_now_ns = ts_now.tv_nsec + ts_now.tv_sec * 1e9;
		uint64_t ts_period_ns = ts_period.tv_nsec + ts_period.tv_sec * 1e9;
		if (ts_now_ns > ts_period_ns) {
			time_t timer;
			char buffer[26];
			struct tm* tm_info;

			timer = time(NULL);
			tm_info = localtime(&timer);
			strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
			printf("%s: going to sleep for %d sec\n", buffer, period);

			ts_period.tv_sec += period;
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts_period, NULL);
			ts_period.tv_sec += period;

			timer = time(NULL);
			tm_info = localtime(&timer);
			strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
			printf("%s: waking from sleep after %d sec\n", buffer, period);
		}

		/* dummy throttle. Sending max size frame takes approx
		 * 12us, so sleep for 110us to acheive approx 10% BW
		 * usage
		 *
		 * Note: this is *very* sensitive to RT-prio for Tx loop!
		 */
		if (throttle)
			usleep(110);
	}

	/* send magic value to stop receiver after waiting a while for
	 * the noise to subside
	 */

	printf("Cooldown period, sending magic terminator in a jiffy\n");
	usleep(100000);
	memset(buffer, 0, payload_size);
	hdr->seqnr = -1;
	int res = sendto(sock, buffer,
			sizeof(*hdr),
			0,
			(struct sockaddr *)&sin,
			sizeof(struct sockaddr_in));
	printf("sent %d with terminator tag\n", res);

	return frames_sent;
}


int main(int argc, char *argv[])
{
	struct argp argp = {
		.options = options,
		.parser = parser
	};

	argp_parse(&argp, argc, argv, 0, NULL, NULL);
	printf("Running %s\n", receiver ? "Receiver" : "Sender");
	printf("Sending %d bytes in each frame to %s\n", sz, ipv4);

	sockfd = create_socket(100000);
	printf("Socket: %d\n", sockfd);

	if (receiver) {
		rx_loop(sockfd, sz);
	} else {
		tx_loop(sockfd, sz);
	}



	if (sockfd > 0)
		close(sockfd);

	return 0;
}
