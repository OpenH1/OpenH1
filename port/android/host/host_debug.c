/*
HOST_DEBUG.C

A sampler for finding where the guest spends its time (or hangs) on a
device without root, where debuggerd cannot attach: with sample_seconds in
config.toml's [debug], every guest thread is interrupted that often and its program
counter, link register and frame chain are written to logcat. The addresses
symbolize against build/android/halo_guest.elf (llvm-symbolizer
--obj=build/android/halo_guest.elf 0x...).
*/

#include "host.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#ifdef __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#include <unistd.h>

#define MAXIMUM_THREADS 64
#define SAMPLE_SIGNAL SIGURG

#ifdef __APPLE__
/* (macOS: threads are named by their pthread_t, interrupted by
pthread_kill; the registers are in the context's mcontext) */
typedef pthread_t thread_id;
#define current_thread() pthread_self()
#define CONTEXT_PC(context) ((const ucontext_t *)(context))->uc_mcontext->__ss.__pc
#define CONTEXT_LR(context) ((const ucontext_t *)(context))->uc_mcontext->__ss.__lr
#define CONTEXT_FP(context) ((const ucontext_t *)(context))->uc_mcontext->__ss.__fp
#define THREAD_FORMAT "%p"
#define THREAD_VALUE(thread) ((void *)(thread))
#else
typedef pid_t thread_id;
#define current_thread() gettid()
#define CONTEXT_PC(context) ((const struct sigcontext *)&((const ucontext_t *)(context))->uc_mcontext)->pc
#define CONTEXT_LR(context) ((const struct sigcontext *)&((const ucontext_t *)(context))->uc_mcontext)->regs[30]
#define CONTEXT_FP(context) ((const struct sigcontext *)&((const ucontext_t *)(context))->uc_mcontext)->regs[29]
#define THREAD_FORMAT "%d"
#define THREAD_VALUE(thread) (thread)
#endif

static thread_id guest_threads[MAXIMUM_THREADS];
static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;

void host_debug_thread_started(void)
{
	int index;

	pthread_mutex_lock(&threads_lock);
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		if (!guest_threads[index])
		{
			guest_threads[index] = current_thread();
			break;
		}
	}
	pthread_mutex_unlock(&threads_lock);
}

void host_debug_thread_exited(void)
{
	thread_id self = current_thread();
	int index;

	pthread_mutex_lock(&threads_lock);
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		if (guest_threads[index] == self)
			guest_threads[index] = 0;
	}
	pthread_mutex_unlock(&threads_lock);
}

static void sample_handler(int signal_number, siginfo_t *information, void *context)
{
	char line[512];
	int length, depth;
	uint64_t fp = CONTEXT_FP(context);

	(void)signal_number;
	(void)information;
	length = snprintf(line, sizeof(line), "sample tid " THREAD_FORMAT ": pc %llx lr %llx", THREAD_VALUE(current_thread()),
		(unsigned long long)CONTEXT_PC(context), (unsigned long long)CONTEXT_LR(context));
	for (depth = 0; depth < 12 && fp && host_in_guest(fp) && !(fp & 7) && host_low_owns(fp, 16); depth++)
	{
		const uint64_t *frame = (const uint64_t *)fp;

		length += snprintf(line + length, sizeof(line) - length, " %llx", (unsigned long long)frame[1]);
		if (frame[0] <= fp)
			break;
		fp = frame[0];
	}
	host_logf(HOST_LOG_INFO, "%s", line);
}

static void *sampler(void *context)
{
	unsigned int seconds = (unsigned int)(uintptr_t)context;

	for (;;)
	{
		thread_id threads[MAXIMUM_THREADS];
		int index;

		sleep(seconds);
		pthread_mutex_lock(&threads_lock);
		memcpy(threads, guest_threads, sizeof(threads));
		pthread_mutex_unlock(&threads_lock);
		for (index = 0; index < MAXIMUM_THREADS; index++)
		{
			if (threads[index])
#ifdef __APPLE__
				pthread_kill(threads[index], SAMPLE_SIGNAL);
#else
				syscall(SYS_tgkill, getpid(), threads[index], SAMPLE_SIGNAL);
#endif
		}
	}
	return NULL;
}

void host_debug_start_sampler(const char *setting)
{
	struct sigaction action;
	pthread_t thread;
	unsigned int seconds = setting ? (unsigned int)atoi(setting) : 0;

	if (!seconds)
		return;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = sample_handler;
	action.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&action.sa_mask);
	sigaction(SAMPLE_SIGNAL, &action, NULL);
	if (pthread_create(&thread, NULL, sampler, (void *)(uintptr_t)seconds) == 0)
		pthread_detach(thread);
	host_logf(HOST_LOG_INFO, "sampling guest threads every %u s", seconds);
}
