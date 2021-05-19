/*
 * Copyright (c)  2021 SINTEF
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */
#define _GNU_SOURCE
#include <argp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/if.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>

/* rusage */
#include <sys/time.h>
#include <sys/resource.h>

#define PTP_MAX_DEV_PATH 16
#define US_IN_S 1000000
#define TIMEOUT_US 1000
#define MS_IN_SEC 1000
#define US_IN_SEC (1000 * MS_IN_SEC)
#define NS_IN_SEC (1000 * US_IN_SEC)
#define OUTFILESZ 256

#include <sched.h>
#include <stdbool.h>

static bool set_rr(int pri)
{
	struct sched_param param;
	param.sched_priority = pri;
	if (sched_setscheduler(0, SCHED_RR, &param)) {
		perror("Failed setting sched_rr");
		return false;
	}
	return true;
}


/*
 * Get clockid, take get_clockid() from
 * https://elixir.bootlin.com/linux/v5.13-rc2/source/tools/testing/selftests/ptp/testptp.c
 */
static clockid_t get_clockid(int fd)
{
#define CLOCKFD 3
#define FD_TO_CLOCKID(fd)	((~(clockid_t) (fd) << 3) | CLOCKFD)

	return FD_TO_CLOCKID(fd);
}

/* read tsc to count cycles */
static inline void get_tsc(unsigned long long *tsc)
{
	unsigned int lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	*tsc = ((unsigned long long)lo) | ((unsigned long long)hi << 32);
}

long long ts_diff(struct timespec *a, struct timespec *b)
{
	long long a_ns = a->tv_sec * NS_IN_SEC + a->tv_nsec;
	long long b_ns = b->tv_sec * NS_IN_SEC + b->tv_nsec;
	return b_ns - a_ns;
}

static char logfile[OUTFILESZ] = { 0 };
static char ifname[IFNAMSIZ] = {0};
static int loops = 1000;
static struct argp_option options[] = {
	{"iface", 'i', "IFACE", 0, "Interface to use" },
	{"loops", 'l', "LOOPS", 0, "Number of loops to run (-1: default, infinite)"},
	{"out", 'o', "OUTFILE", 0, "File to store output to (csv-format)"},
	{0}
};
static error_t parser(int key, char *arg, struct argp_state *state)
{
	int tmp;

	switch(key) {
	case 'i':
		strncpy(ifname, arg, IFNAMSIZ-1);
		break;
	case 'l':
		tmp = atoi(arg);
		if (tmp < -1 || tmp > 1e8) {
			fprintf(stderr, "Invalid number of loops, ignoring (using %d)\n", loops);
			break;
		}
		loops = tmp;
		break;
	case 'o':
		strncpy(logfile, arg, OUTFILESZ-1);
		break;
	}
	return 0;
}
static struct argp argp = { options, parser };

int main(int argc, char *argv[])
{
	int ret = 0;

	argp_parse(&argp, argc, argv, 0, NULL, NULL);

	if (strlen(ifname) == 0)
		strncpy(ifname, "eth2", IFNAMSIZ-1);

	if (strlen(logfile) == 0)
		strncpy(logfile, "hack_ptp.csv", OUTFILESZ-1);

	printf("Using iface %s \n", ifname);

	/* Get PHC index */
	struct ifreq req;
	struct ethtool_ts_info interface_info = {0};
	interface_info.cmd = ETHTOOL_GET_TS_INFO;
	snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", ifname);
	req.ifr_data = (char *) &interface_info;

	int ioctl_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (ioctl_fd < 0) {
		perror("Failed opening fd for ioctl\n");
		exit(EXIT_FAILURE);
	}
	if (ioctl(ioctl_fd, SIOCETHTOOL, &req) < 0) {
		perror("ioctl failed");
		exit(EXIT_FAILURE);
	}
	if (interface_info.phc_index < 0) {
		fprintf(stderr, "No suitable PTP device found for nic %s\n", ifname);
		exit(EXIT_FAILURE);
	}
	close(ioctl_fd);

	char ptp_path[PTP_MAX_DEV_PATH] = {0};
	snprintf(ptp_path, sizeof(ptp_path), "%s%d", "/dev/ptp",
		interface_info.phc_index);
	printf("ptp_path: %s\n", ptp_path);

	int ptp_fd = open(ptp_path, O_RDONLY);
	if (ptp_fd < 0) {
		perror("Failed opening PTP fd, perhaps try with sudo?");
		exit(EXIT_FAILURE);
	}

	struct timespec ts_ptp;
	struct timespec ts_mono;
	unsigned long long start, end, diff_mono, diff_ptp;
	FILE *fp = NULL;
	if (strlen(logfile) > 0) {
		fp = fopen(logfile, "w+");
		if (!fp) {
			perror("Failed opening file");
			exit(EXIT_FAILURE);
		}
		fprintf(fp, "clock_realtime_s,tsc_real,tsc_ptp,clock_diff_s\n");
	}

	if (!set_rr(80))
		exit(EXIT_FAILURE);

	for (size_t i = 0; i < loops; i++) {
		/* Get rusage */
		struct rusage rstart,rend;
		if (getrusage(RUSAGE_THREAD, &rstart)) {
			perror("failed to get rusage (start), breaking loop");
			ret = -1;
			loops = -1;
			continue;
		}

		get_tsc(&start);
		clock_gettime(CLOCK_REALTIME, &ts_mono);
		get_tsc(&end);
		diff_mono = end-start;

		get_tsc(&start);
		clock_gettime(get_clockid(ptp_fd), &ts_ptp);
		get_tsc(&end);
		diff_ptp = end-start;

		/* Get rusage, make sure we haven't been preempted */
		if (getrusage(RUSAGE_THREAD, &rend)) {
			perror("failed to get rusage (end), breaking loop");
			ret = -1;
			loops = -1;
			continue;
		}
		if (rstart.ru_nsignals != rend.ru_nsignals ||
			rstart.ru_nvcsw != rend.ru_nvcsw ||
			rstart.ru_nivcsw != rend.ru_nivcsw) {
			fprintf(stderr, "signals or contextswitches increaded during run, ignoring this run\n");
			continue;
		}

		long long d = ts_diff(&ts_mono, &ts_ptp);
		if (fp > 0)
			fprintf(fp, "%ld.%ld,%llu,%llu,%f\n", ts_mono.tv_sec, ts_mono.tv_nsec, diff_mono, diff_ptp, d/1e9);

		if (!(i%(US_IN_S / TIMEOUT_US))) {
			printf("real: %lu %lu\nptp : %lu %lu\ndiff: %lld\n",
				ts_mono.tv_sec, ts_mono.tv_nsec,
				ts_ptp.tv_sec, ts_ptp.tv_nsec,
				d);
		}

		usleep(TIMEOUT_US);
	}

	fclose(fp);
	close(ptp_fd);
	return ret;
}
