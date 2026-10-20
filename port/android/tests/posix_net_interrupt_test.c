/* The sockets' select (port/linux/src/posix_net.c) under signals: Winsock's
is never interrupted, and the game takes a failed one as the connection
lost, so a select interrupted by signals (as the host's sampler sends a
thousand a second here) waits out its time and fails with nothing. */

#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "posix.h"

static volatile sig_atomic_t stop;
static volatile sig_atomic_t signals;
static pthread_t target;

static void handler(int signal_number)
{
	(void)signal_number;
	signals++;
}

static void *signaller(void *context)
{
	struct timespec interval = { 0, 1000000 };

	(void)context;
	while (!stop)
	{
		pthread_kill(target, SIGURG);
		nanosleep(&interval, NULL);
	}
	return NULL;
}

static long milliseconds_between(const struct timespec *start, const struct timespec *end)
{
	return (long)((end->tv_sec - start->tv_sec) * 1000 + (end->tv_nsec - start->tv_nsec) / 1000000);
}

int main(void)
{
	struct sigaction action;
	struct timespec start, end;
	pthread_t thread;
	int descriptor, count = 1, result, error;
	long waited;

	memset(&action, 0, sizeof(action));
	action.sa_handler = handler;
	/* (as the sampler's: poll is interrupted all the same) */
	action.sa_flags = SA_RESTART;
	sigemptyset(&action.sa_mask);
	sigaction(SIGURG, &action, NULL);
	descriptor = socket(AF_INET, SOCK_DGRAM, 0);
	if (descriptor < 0)
	{
		perror("socket");
		return 1;
	}
	target = pthread_self();
	pthread_create(&thread, NULL, signaller, NULL);
	clock_gettime(CLOCK_MONOTONIC, &start);
	result = posix_socket_select(&descriptor, &count, NULL, NULL, NULL, NULL, 0, 300000, 0);
	error = posix_socket_last_error();
	clock_gettime(CLOCK_MONOTONIC, &end);
	stop = 1;
	pthread_join(thread, NULL);
	close(descriptor);
	waited = milliseconds_between(&start, &end);
	if (result != 0 || count != 0 || waited < 290 || signals < 10)
	{
		printf("select under signals: result %d, count %d, last error %d, waited %ld ms of 300, %d signals\n",
			result, count, error, waited, (int)signals);
		return 1;
	}
	return 0;
}
