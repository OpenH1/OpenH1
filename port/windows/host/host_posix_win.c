/*
HOST_POSIX_WIN.C

The POSIX functions of port/windows/host/posix (threads, clocks, sleeps,
anonymous memory) over Windows, for the host files the 64-bit Windows host
shares with the Linux and macOS hosts (port/android/host); and the host's
text and error conversions (host_win.h).
*/

#include "host_win.h"

#include <windows.h>
#include <direct.h>
#include <io.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ---------- threads

pthread_t is the thread's identifier; the handle of a joinable thread is
kept here, in a list of entries each made before its thread (so that every
joinable thread can be joined), until pthread_join or pthread_detach closes
it (while it is open, Windows gives no other thread that identifier). */

struct joinable_thread
{
	struct joinable_thread *next;
	pthread_t thread;
	HANDLE handle;
};

static struct joinable_thread *joinable;
static SRWLOCK joinable_lock = SRWLOCK_INIT;

struct thread_start
{
	void *(*start)(void *);
	void *argument;
};

static DWORD WINAPI thread_main(LPVOID parameter)
{
	struct thread_start start = *(struct thread_start *)parameter;

	free(parameter);
	start.start(start.argument);
	return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *), void *argument)
{
	struct thread_start *context = malloc(sizeof(*context));
	int detached = attributes && attributes->detached;
	struct joinable_thread *entry = detached ? NULL : malloc(sizeof(*entry));
	HANDLE handle;
	DWORD identifier;

	if (!context || (!detached && !entry))
	{
		free(context);
		free(entry);
		return EAGAIN;
	}
	context->start = start;
	context->argument = argument;
	handle = CreateThread(NULL, attributes ? attributes->stack_size : 0, thread_main, context,
		STACK_SIZE_PARAM_IS_A_RESERVATION, &identifier);
	if (!handle)
	{
		free(context);
		free(entry);
		return EAGAIN;
	}
	*thread = identifier;
	if (detached)
	{
		CloseHandle(handle);
		return 0;
	}
	entry->thread = identifier;
	entry->handle = handle;
	AcquireSRWLockExclusive(&joinable_lock);
	entry->next = joinable;
	joinable = entry;
	ReleaseSRWLockExclusive(&joinable_lock);
	return 0;
}

/* the joinable thread's handle, taken out of the list; NULL if none */
static HANDLE joinable_take(pthread_t thread)
{
	struct joinable_thread **link, *entry = NULL;
	HANDLE handle = NULL;

	AcquireSRWLockExclusive(&joinable_lock);
	for (link = &joinable; *link; link = &(*link)->next)
	{
		if ((*link)->thread == thread)
		{
			entry = *link;
			*link = entry->next;
			break;
		}
	}
	ReleaseSRWLockExclusive(&joinable_lock);
	if (entry)
	{
		handle = entry->handle;
		free(entry);
	}
	return handle;
}

int pthread_join(pthread_t thread, void **result)
{
	HANDLE handle = joinable_take(thread);

	if (!handle)
		return ESRCH;
	WaitForSingleObject(handle, INFINITE);
	CloseHandle(handle);
	if (result)
		*result = NULL;
	return 0;
}

int pthread_detach(pthread_t thread)
{
	HANDLE handle = joinable_take(thread);

	if (!handle)
		return ESRCH;
	CloseHandle(handle);
	return 0;
}

pthread_t pthread_self(void)
{
	return GetCurrentThreadId();
}

int pthread_equal(pthread_t thread1, pthread_t thread2)
{
	return thread1 == thread2;
}

int pthread_attr_init(pthread_attr_t *attributes)
{
	attributes->stack_size = 0;
	attributes->detached = 0;
	return 0;
}

int pthread_attr_destroy(pthread_attr_t *attributes)
{
	(void)attributes;
	return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attributes, int state)
{
	attributes->detached = state == PTHREAD_CREATE_DETACHED;
	return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attributes, size_t size)
{
	attributes->stack_size = size;
	return 0;
}

/* ---------- mutexes and condition variables */

_Static_assert(sizeof(SRWLOCK) == sizeof(void *), "an SRWLOCK is a pointer");
_Static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void *), "a CONDITION_VARIABLE is a pointer");

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes)
{
	(void)attributes;
	InitializeSRWLock((SRWLOCK *)&mutex->lock);
	return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	(void)mutex;
	return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	AcquireSRWLockExclusive((SRWLOCK *)&mutex->lock);
	return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	return TryAcquireSRWLockExclusive((SRWLOCK *)&mutex->lock) ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	ReleaseSRWLockExclusive((SRWLOCK *)&mutex->lock);
	return 0;
}

int pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes)
{
	(void)attributes;
	InitializeConditionVariable((CONDITION_VARIABLE *)&condition->variable);
	return 0;
}

int pthread_cond_destroy(pthread_cond_t *condition)
{
	(void)condition;
	return 0;
}

int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
	SleepConditionVariableSRW((CONDITION_VARIABLE *)&condition->variable, (SRWLOCK *)&mutex->lock, INFINITE, 0);
	return 0;
}

int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex, const struct timespec *deadline)
{
	struct timespec now;
	long long milliseconds;

	clock_gettime(CLOCK_REALTIME, &now);
	milliseconds = (long long)(deadline->tv_sec - now.tv_sec) * 1000 +
		((long long)deadline->tv_nsec - now.tv_nsec + 999999) / 1000000;
	if (milliseconds <= 0)
		return ETIMEDOUT;
	if (milliseconds >= INFINITE)
		milliseconds = INFINITE - 1;
	if (!SleepConditionVariableSRW((CONDITION_VARIABLE *)&condition->variable, (SRWLOCK *)&mutex->lock,
		(DWORD)milliseconds, 0))
	{
		return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
	}
	return 0;
}

int pthread_cond_signal(pthread_cond_t *condition)
{
	WakeConditionVariable((CONDITION_VARIABLE *)&condition->variable);
	return 0;
}

int pthread_cond_broadcast(pthread_cond_t *condition)
{
	WakeAllConditionVariable((CONDITION_VARIABLE *)&condition->variable);
	return 0;
}

/* ---------- clocks and sleeps */

/* 100-nanosecond intervals between 1601 (FILETIME's epoch) and 1970 */
#define FILETIME_UNIX_EPOCH 116444736000000000ULL

void host_unix_time_of_filetime(uint64_t filetime, int64_t *seconds, int64_t *nanoseconds)
{
	int64_t value = (int64_t)filetime - (int64_t)FILETIME_UNIX_EPOCH;

	*seconds = value / 10000000;
	*nanoseconds = (value % 10000000) * 100;
	if (*nanoseconds < 0)
	{
		*seconds -= 1;
		*nanoseconds += 1000000000;
	}
}

uint64_t host_filetime_of_unix_time(int64_t seconds, int64_t nanoseconds)
{
	return (uint64_t)(seconds * 10000000 + nanoseconds / 100) + FILETIME_UNIX_EPOCH;
}

static uint64_t filetime_value(const FILETIME *time)
{
	return ((uint64_t)time->dwHighDateTime << 32) | time->dwLowDateTime;
}

static void timespec_of_100ns(uint64_t value, struct timespec *time)
{
	time->tv_sec = (time_t)(value / 10000000ULL);
	time->tv_nsec = (long)(value % 10000000ULL) * 100;
}

int clock_gettime(clockid_t clock, struct timespec *time)
{
	switch (clock)
	{
	case CLOCK_REALTIME:
	{
		FILETIME now;

		int64_t seconds, nanoseconds;

		GetSystemTimePreciseAsFileTime(&now);
		host_unix_time_of_filetime(filetime_value(&now), &seconds, &nanoseconds);
		time->tv_sec = (time_t)seconds;
		time->tv_nsec = (long)nanoseconds;
		return 0;
	}
	case CLOCK_MONOTONIC:
	{
		static LARGE_INTEGER frequency;
		LARGE_INTEGER counter;

		if (!frequency.QuadPart)
			QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&counter);
		time->tv_sec = (time_t)(counter.QuadPart / frequency.QuadPart);
		time->tv_nsec = (long)((counter.QuadPart % frequency.QuadPart) * 1000000000LL / frequency.QuadPart);
		return 0;
	}
	case CLOCK_PROCESS_CPUTIME_ID:
	case CLOCK_THREAD_CPUTIME_ID:
	{
		FILETIME created, exited, kernel, user;
		BOOL result = clock == CLOCK_PROCESS_CPUTIME_ID ?
			GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) :
			GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);

		if (!result)
			break;
		timespec_of_100ns(filetime_value(&kernel) + filetime_value(&user), time);
		return 0;
	}
	}
	errno = EINVAL;
	return -1;
}

int clock_getres(clockid_t clock, struct timespec *resolution)
{
	if (clock < CLOCK_REALTIME || clock > CLOCK_THREAD_CPUTIME_ID)
	{
		errno = EINVAL;
		return -1;
	}
	resolution->tv_sec = 0;
	resolution->tv_nsec = clock == CLOCK_MONOTONIC ? 100 : clock == CLOCK_REALTIME ? 100 : 15625000;
	return 0;
}

static uint64_t nanoseconds_of(const struct timespec *time)
{
	if (time->tv_sec < 0)
		return 0;
	return (uint64_t)time->tv_sec * 1000000000ULL + (uint64_t)time->tv_nsec;
}

int nanosleep(const struct timespec *duration, struct timespec *remaining)
{
	if (duration->tv_nsec < 0 || duration->tv_nsec >= 1000000000L)
	{
		errno = EINVAL;
		return -1;
	}
	host_sleep_nanoseconds(nanoseconds_of(duration));
	if (remaining)
	{
		remaining->tv_sec = 0;
		remaining->tv_nsec = 0;
	}
	return 0;
}

int clock_nanosleep(clockid_t clock, int flags, const struct timespec *time, struct timespec *remaining)
{
	uint64_t wait = nanoseconds_of(time);

	if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)
		return EINVAL;
	if (time->tv_nsec < 0 || time->tv_nsec >= 1000000000L)
		return EINVAL;
	if (flags & TIMER_ABSTIME)
	{
		struct timespec now;
		uint64_t current;

		clock_gettime(clock, &now);
		current = nanoseconds_of(&now);
		wait = wait > current ? wait - current : 0;
	}
	host_sleep_nanoseconds(wait);
	if (remaining && !(flags & TIMER_ABSTIME))
	{
		remaining->tv_sec = 0;
		remaining->tv_nsec = 0;
	}
	return 0;
}

struct tm *localtime_r(const time_t *time, struct tm *result)
{
	return localtime_s(result, time) == 0 ? result : NULL;
}

struct tm *gmtime_r(const time_t *time, struct tm *result)
{
	return gmtime_s(result, time) == 0 ? result : NULL;
}

/* ---------- the page size, the processors, folders */

int getpagesize(void)
{
	SYSTEM_INFO information;

	GetSystemInfo(&information);
	return (int)information.dwPageSize;
}

long sysconf(int name)
{
	SYSTEM_INFO information;

	GetSystemInfo(&information);
	switch (name)
	{
	case _SC_PAGESIZE:
		return (long)information.dwPageSize;
	case _SC_NPROCESSORS_ONLN:
		return (long)information.dwNumberOfProcessors;
	}
	errno = EINVAL;
	return -1;
}

int halo_windows_mkdir(const char *path, int mode)
{
	wchar_t *wide = host_wide_from_utf8(path);
	int result;

	(void)mode;
	if (!wide)
		return -1;
	result = _wmkdir(wide);
	free(wide);
	return result;
}

/* ---------- anonymous memory */

unsigned long host_windows_protection(int protection)
{
	switch (protection & (PROT_READ | PROT_WRITE | PROT_EXEC))
	{
	case PROT_NONE:
		return PAGE_NOACCESS;
	case PROT_READ:
		return PAGE_READONLY;
	case PROT_EXEC:
		return PAGE_EXECUTE;
	case PROT_READ | PROT_EXEC:
		return PAGE_EXECUTE_READ;
	case PROT_WRITE | PROT_EXEC:
	case PROT_READ | PROT_WRITE | PROT_EXEC:
		return PAGE_EXECUTE_READWRITE;
	default:
		/* (write without read is read-write, as on x86's pages) */
		return PAGE_READWRITE;
	}
}

void *mmap(void *address, size_t length, int protection, int flags, int descriptor, long long offset)
{
	void *result;

	(void)offset;
	if (descriptor != -1 || !(flags & MAP_ANONYMOUS) || !length)
	{
		errno = EINVAL;
		return MAP_FAILED;
	}
	if (flags & MAP_FIXED)
	{
		/* (address space the host reserved) */
		result = VirtualAlloc(address, length, MEM_COMMIT, host_windows_protection(protection));
		if (result != address)
		{
			errno = ENOMEM;
			return MAP_FAILED;
		}
		return result;
	}
	result = VirtualAlloc(NULL, length, MEM_RESERVE | MEM_COMMIT, host_windows_protection(protection));
	if (!result)
	{
		errno = ENOMEM;
		return MAP_FAILED;
	}
	return result;
}

int munmap(void *address, size_t length)
{
	MEMORY_BASIC_INFORMATION region;

	if (!VirtualQuery(address, &region, sizeof(region)))
	{
		errno = EINVAL;
		return -1;
	}
	/* (an allocation of mmap's own goes whole; pages of a reservation go
	back to it) */
	if (region.AllocationBase == address && region.RegionSize >= length &&
		VirtualFree(address, 0, MEM_RELEASE))
	{
		return 0;
	}
	if (!VirtualFree(address, length, MEM_DECOMMIT))
	{
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int mprotect(void *address, size_t length, int protection)
{
	DWORD previous;

	if (!VirtualProtect(address, length, host_windows_protection(protection), &previous))
	{
		errno = host_linux_errno_of_windows(GetLastError()) == ENOMEM ? ENOMEM : EACCES;
		return -1;
	}
	return 0;
}

int mincore(void *address, size_t length, unsigned char *residency)
{
	uintptr_t at = (uintptr_t)address & ~(uintptr_t)0xfff, end = (uintptr_t)address + length;
	size_t page = 0;

	while (at < end)
	{
		MEMORY_BASIC_INFORMATION region;
		uintptr_t region_end;

		if (!VirtualQuery((void *)at, &region, sizeof(region)) || region.State != MEM_COMMIT ||
			(region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !region.Protect)
		{
			errno = ENOMEM;
			return -1;
		}
		region_end = (uintptr_t)region.BaseAddress + region.RegionSize;
		for (; at < end && at < region_end; at += 0x1000)
			residency[page++] = 1;
	}
	return 0;
}

/* ---------- the C runtime's descriptors

Each call is made with a thread-local invalid-parameter handler that does
nothing, so that a descriptor that is not open fails with EBADF (the guest's
close or dup of one, as on Linux) rather than ending the game: the global
handler reports it as a crash (host_crash_win.c). */

static void quiet_invalid_parameter(const wchar_t *expression, const wchar_t *function, const wchar_t *file,
	unsigned int line, uintptr_t reserved)
{
	(void)expression;
	(void)function;
	(void)file;
	(void)line;
	(void)reserved;
}

intptr_t host_crt_osfhandle(int fd)
{
	_invalid_parameter_handler previous;
	intptr_t handle;

	if (fd < 0)
		return (intptr_t)INVALID_HANDLE_VALUE;
	previous = _set_thread_local_invalid_parameter_handler(quiet_invalid_parameter);
	handle = _get_osfhandle(fd);
	_set_thread_local_invalid_parameter_handler(previous);
	return handle;
}

int host_crt_close(int fd)
{
	_invalid_parameter_handler previous = _set_thread_local_invalid_parameter_handler(quiet_invalid_parameter);
	int result = _close(fd);

	_set_thread_local_invalid_parameter_handler(previous);
	return result;
}

int host_crt_dup(int fd)
{
	_invalid_parameter_handler previous = _set_thread_local_invalid_parameter_handler(quiet_invalid_parameter);
	int result = _dup(fd);

	_set_thread_local_invalid_parameter_handler(previous);
	return result;
}

int host_crt_dup2(int from, int to)
{
	_invalid_parameter_handler previous = _set_thread_local_invalid_parameter_handler(quiet_invalid_parameter);
	int result = _dup2(from, to);

	_set_thread_local_invalid_parameter_handler(previous);
	return result;
}

/* ---------- text */

wchar_t *host_wide_from_utf8(const char *text)
{
	int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	wchar_t *wide;

	if (length <= 0)
	{
		errno = EINVAL;
		return NULL;
	}
	wide = malloc((size_t)length * sizeof(wchar_t));
	if (!wide)
	{
		errno = ENOMEM;
		return NULL;
	}
	MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, length);
	return wide;
}

char *host_utf8_from_wide(const wchar_t *text)
{
	int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
	char *utf8;

	if (length <= 0)
	{
		errno = EINVAL;
		return NULL;
	}
	utf8 = malloc((size_t)length);
	if (!utf8)
	{
		errno = ENOMEM;
		return NULL;
	}
	WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, length, NULL, NULL);
	return utf8;
}

/* ---------- errors (Linux's numbers: halo_guest_syscall.h, and musl's) */

int host_linux_errno_of_windows(unsigned long error)
{
	switch (error)
	{
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_DRIVE:
	case ERROR_BAD_NETPATH:
	case ERROR_BAD_NET_NAME:
	case ERROR_INVALID_NAME:
	case ERROR_BAD_PATHNAME:
	case ERROR_MOD_NOT_FOUND:
		return 2; /* ENOENT */
	case ERROR_TOO_MANY_OPEN_FILES:
		return 24; /* EMFILE */
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
	case ERROR_WRITE_PROTECT:
	case ERROR_CURRENT_DIRECTORY:
	case ERROR_PRIVILEGE_NOT_HELD:
		return 13; /* EACCES */
	case ERROR_INVALID_HANDLE:
		return 9; /* EBADF */
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
	case ERROR_COMMITMENT_LIMIT:
		return 12; /* ENOMEM */
	case ERROR_NOACCESS:
		return 14; /* EFAULT */
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:
		return 17; /* EEXIST */
	case ERROR_NOT_SAME_DEVICE:
		return 18; /* EXDEV */
	case ERROR_DIRECTORY:
		return 20; /* ENOTDIR */
	case ERROR_INVALID_PARAMETER:
	case ERROR_INVALID_FUNCTION:
	case ERROR_NEGATIVE_SEEK:
		return 22; /* EINVAL */
	case ERROR_HANDLE_DISK_FULL:
	case ERROR_DISK_FULL:
		return 28; /* ENOSPC */
	case ERROR_BROKEN_PIPE:
	case ERROR_NO_DATA:
		return 32; /* EPIPE */
	case ERROR_FILENAME_EXCED_RANGE:
		return 36; /* ENAMETOOLONG */
	case ERROR_DIR_NOT_EMPTY:
		return 39; /* ENOTEMPTY */
	case ERROR_NOT_SUPPORTED:
		return 95; /* EOPNOTSUPP */
	case ERROR_TIMEOUT:
	case WAIT_TIMEOUT:
		return 110; /* ETIMEDOUT */
	case ERROR_IO_PENDING:
		return 11; /* EAGAIN */
	}
	return 5; /* EIO */
}

int host_linux_errno(int error)
{
	/* (the C runtime's numbers are Linux's to ERANGE, 34; its POSIX
	supplement's are not) */
	switch (error)
	{
	case EDEADLK: return 35;
	case ENAMETOOLONG: return 36;
	case ENOLCK: return 37;
	case ENOSYS: return 38;
	case ENOTEMPTY: return 39;
	case EILSEQ: return 84;
	case EADDRINUSE: return 98;
	case EADDRNOTAVAIL: return 99;
	case EAFNOSUPPORT: return 97;
	case EALREADY: return 114;
	case ECONNABORTED: return 103;
	case ECONNREFUSED: return 111;
	case ECONNRESET: return 104;
	case EINPROGRESS: return 115;
	case EISCONN: return 106;
	case ELOOP: return 40;
	case EMSGSIZE: return 90;
	case ENETDOWN: return 100;
	case ENETUNREACH: return 101;
	case ENOBUFS: return 105;
	case ENOTCONN: return 107;
	case ENOTSOCK: return 88;
	case EOPNOTSUPP: return 95;
	case EOVERFLOW: return 75;
	case ETIMEDOUT: return 110;
	case EWOULDBLOCK: return 11;
	}
	return error;
}
