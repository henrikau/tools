#include "ftrace.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

static bool ftrace_ready = false;

static FILE *marker_fp = NULL;
static FILE *enable_fp = NULL;

#define FPATH "/sys/kernel/debug/tracing"

bool enable_ftrace(void)
{
	/* find ftrace dir */
	if (access(FPATH, R_OK)) {
		perror("Could not read path");
		return false;
	}

	FILE *fp = NULL;

	/* configure */
	fp = fopen(FPATH "/buffer_size_kb", "w");
	if (fp) {
		fprintf(fp, "2048\n");
		fclose(fp);
	}
	fp = fopen(FPATH "/events/sched/enable", "w");
	if (fp) {
		fprintf(fp, "1\n");
		fclose(fp);
	}
	fp = fopen(FPATH "/events/irq/enable", "w");
	if (fp) {
		fprintf(fp, "1\n");
		fclose(fp);
	}

	/* open trace_marker */
	marker_fp = fopen(FPATH "/trace_marker", "w");
	enable_fp = fopen(FPATH "/tracing_on", "w");

	if (!marker_fp || !enable_fp) {
		ftrace_ready = false;
		return false;
	}
	ftrace_ready = true;
	return true;
}

bool start_ftrace(void)
{
	if (!ftrace_ready)
		return true;

	/* 1 > tracing_on */
	if (!marker_fp || !enable_fp)
		return false;

	fprintf(marker_fp, "\n"); fflush(marker_fp);
	fprintf(enable_fp, "1\n"); fflush(enable_fp);
	return true;
}

void tag_ftrace(const char *msg)
{
	if (!ftrace_ready)
		return;
	fprintf(marker_fp, "%s\n", msg);
	fflush(marker_fp);
	return;
}

void stop_ftrace(void)
{
	if (!ftrace_ready)
		return ;

	/* racy, but we should be single threaded */
	ftrace_ready = false;

	fprintf(enable_fp, "0\n");
	fflush(enable_fp);

	fclose(enable_fp);
	enable_fp = NULL;
	fclose(marker_fp);
	marker_fp = NULL;

	return ;
}
