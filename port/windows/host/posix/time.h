/*
TIME.H

The C runtime's <time.h> and the POSIX clocks and sleeps the 64-bit
Windows host's shared files use (port/windows/host/host_posix_win.c).
*/

#ifndef __HALO_WINDOWS_HOST_TIME_H
#define __HALO_WINDOWS_HOST_TIME_H

#include_next <time.h>

typedef int clockid_t;

/* (Linux's numbers) */
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3

#define TIMER_ABSTIME 1

int clock_gettime(clockid_t clock, struct timespec *time);
int clock_getres(clockid_t clock, struct timespec *resolution);
int nanosleep(const struct timespec *duration, struct timespec *remaining);
/* CLOCK_REALTIME or CLOCK_MONOTONIC, relative or TIMER_ABSTIME; 0 or an
errno value */
int clock_nanosleep(clockid_t clock, int flags, const struct timespec *time, struct timespec *remaining);
struct tm *localtime_r(const time_t *time, struct tm *result);
struct tm *gmtime_r(const time_t *time, struct tm *result);

#endif
