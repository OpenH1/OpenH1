/* Opt-in loading diagnostics. Times share CLOCK_MONOTONIC with the guest.
Submission and swap events are not proof of physical display presentation. */
#ifndef HOST_LOAD_TRACE_H
#define HOST_LOAD_TRACE_H
#include "host.h"
#include <pthread.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static inline int host_load_trace_enabled(void)
{
	const char *value = getenv("HALO_LOAD_TRACE");

	return value && *value && strcmp(value, "0") && strcmp(value, "false");
}

static inline double host_load_trace_clock(clockid_t clock)
{
	struct timespec value;

	return clock_gettime(clock, &value) == 0 ? (double)value.tv_sec + value.tv_nsec / 1e9 : -1.0;
}

static inline int host_load_trace_detail_enabled(void)
{
	const char *value = getenv("HALO_LOAD_TRACE_DETAIL");

	return host_load_trace_enabled() && value && *value && strcmp(value, "0") && strcmp(value, "false");
}

static inline void host_load_trace(const char *phase, const char *format, ...)
{
	double wall, thread, process;
	char detail[256];
	va_list arguments;

	if (!host_load_trace_enabled())
		return;
	wall = host_load_trace_clock(CLOCK_MONOTONIC);
	thread = host_load_trace_clock(CLOCK_THREAD_CPUTIME_ID);
	process = host_load_trace_clock(CLOCK_PROCESS_CPUTIME_ID);
	va_start(arguments, format);
	vsnprintf(detail, sizeof(detail), format, arguments);
	va_end(arguments);
	/* pthread identity is local to this host trace, not an SDL thread ID. */
	host_logf(HOST_LOG_INFO, "LOAD phase=%s wall=%.6f thread=%.6f process=%.6f owner=host:%llx detail=%s",
		phase, wall, thread, process, (unsigned long long)(uintptr_t)pthread_self(), detail);
}
#endif
