/*
PTHREAD.H

The POSIX threads the 64-bit Windows host's shared files use
(port/android/host: the render thread, its recording's writer, the audio
callbacks' hand-over), over Windows's threads, slim reader/writer locks and
condition variables (port/windows/host/host_posix_win.c). The host's own
threads that run guest code are host_thread_win.c's.

A mutex is not recursive (an SRWLOCK), which none of those files needs; a
static initializer is all zeros, as SRWLOCK_INIT and
CONDITION_VARIABLE_INIT are.
*/

#ifndef __HALO_WINDOWS_HOST_PTHREAD_H
#define __HALO_WINDOWS_HOST_PTHREAD_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* the thread's identifier (GetCurrentThreadId) */
typedef uintptr_t pthread_t;

typedef struct
{
	void *lock; /* an SRWLOCK */
} pthread_mutex_t;

typedef struct
{
	void *variable; /* a CONDITION_VARIABLE */
} pthread_cond_t;

typedef struct
{
	size_t stack_size;
	int detached;
} pthread_attr_t;

typedef struct
{
	int type;
} pthread_mutexattr_t;

typedef struct
{
	int unused;
} pthread_condattr_t;

#define PTHREAD_MUTEX_INITIALIZER { 0 }
#define PTHREAD_COND_INITIALIZER { 0 }

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *), void *argument);
int pthread_join(pthread_t thread, void **result);
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t thread1, pthread_t thread2);

int pthread_attr_init(pthread_attr_t *attributes);
int pthread_attr_destroy(pthread_attr_t *attributes);
int pthread_attr_setdetachstate(pthread_attr_t *attributes, int state);
int pthread_attr_setstacksize(pthread_attr_t *attributes, size_t size);

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

int pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes);
int pthread_cond_destroy(pthread_cond_t *condition);
int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex);
/* deadline on the CLOCK_REALTIME clock; ETIMEDOUT once it passes */
int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex, const struct timespec *deadline);
int pthread_cond_signal(pthread_cond_t *condition);
int pthread_cond_broadcast(pthread_cond_t *condition);

#endif
