/*
HOST_DEBUG_WIN.C

The sampler of the guest's threads on Windows (config.toml's
debug.sample_seconds, or HALO_SAMPLE; port/android/host/host_debug.c is the
Linux and macOS hosts'): every interval, each thread that runs guest code
is suspended, its program counter and frame records logged, and resumed.
Each record is copied with ReadProcessMemory, which fails rather than
faulting on memory that cannot be read, as the frame pointer may hold
anything in code built without frame pointers.
*/

#include "host_win.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXIMUM_THREADS 64

static DWORD guest_threads[MAXIMUM_THREADS];
static SRWLOCK threads_lock = SRWLOCK_INIT;

void host_debug_thread_started(void)
{
	static int overflow_logged;
	DWORD self = GetCurrentThreadId();
	int index;

	AcquireSRWLockExclusive(&threads_lock);
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		if (!guest_threads[index])
		{
			guest_threads[index] = self;
			break;
		}
	}
	if (index == MAXIMUM_THREADS && !overflow_logged)
	{
		overflow_logged = 1;
		host_logf(HOST_LOG_WARN, "more than %d guest threads: the sampler leaves the others out", MAXIMUM_THREADS);
	}
	ReleaseSRWLockExclusive(&threads_lock);
}

void host_debug_thread_exited(void)
{
	DWORD self = GetCurrentThreadId();
	int index;

	AcquireSRWLockExclusive(&threads_lock);
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		if (guest_threads[index] == self)
			guest_threads[index] = 0;
	}
	ReleaseSRWLockExclusive(&threads_lock);
}

static int read_memory(uintptr_t address, void *buffer, size_t size)
{
	SIZE_T copied = 0;

	return ReadProcessMemory(GetCurrentProcess(), (const void *)address, buffer, size, &copied) && copied == size;
}

static void sample(DWORD identifier)
{
	HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, identifier);
	CONTEXT context;
	char line[512];
	uint64_t fp;
	int length, count;

	if (!thread)
		return;
	if (SuspendThread(thread) == (DWORD)-1)
	{
		CloseHandle(thread);
		return;
	}
	context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	if (GetThreadContext(thread, &context))
	{
		length = snprintf(line, sizeof(line), "sample tid %lu: pc %llx", (unsigned long)identifier,
			(unsigned long long)context.Rip);
		/* (the frame records: the caller's rbp, then the return address) */
		fp = context.Rbp;
		for (count = 0; count < 12 && fp && !(fp & 7) && length > 0 && (size_t)length < sizeof(line); count++)
		{
			uint64_t frame[2];

			if (!read_memory((uintptr_t)fp, frame, sizeof(frame)))
				break;
			length += snprintf(line + length, sizeof(line) - length, " %llx", (unsigned long long)frame[1]);
			if (frame[0] <= fp)
				break;
			fp = frame[0];
		}
		ResumeThread(thread);
		host_log(HOST_LOG_INFO, line);
	}
	else
	{
		ResumeThread(thread);
	}
	CloseHandle(thread);
}

static uint64_t sample_interval;

static DWORD WINAPI sampler(LPVOID unused)
{
	(void)unused;
	for (;;)
	{
		DWORD threads[MAXIMUM_THREADS];
		int index;

		host_sleep_nanoseconds(sample_interval);
		AcquireSRWLockShared(&threads_lock);
		memcpy(threads, guest_threads, sizeof(threads));
		ReleaseSRWLockShared(&threads_lock);
		for (index = 0; index < MAXIMUM_THREADS; index++)
		{
			if (threads[index])
				sample(threads[index]);
		}
	}
	return 0;
}

void host_debug_start_sampler(const char *setting)
{
	/* (a real number of seconds, as the setting is: 0.5 is twice a second) */
	double seconds = setting ? strtod(setting, NULL) : 0.0;
	HANDLE thread;

	if (!(seconds > 0.0))
		return;
	/* (at most a hundred samples a second, and at least one a day) */
	if (seconds < 0.01)
		seconds = 0.01;
	if (seconds > 86400.0)
		seconds = 86400.0;
	sample_interval = (uint64_t)(seconds * 1e9);
	thread = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
	if (thread)
		CloseHandle(thread);
	host_logf(HOST_LOG_INFO, "sampling guest threads every %g s", seconds);
}
