/*
UNISTD.H

The POSIX names the 64-bit Windows host's shared files use: the C
runtime's descriptors (<io.h>), and the page size and sysconf of
port/windows/host/host_posix_win.c.
*/

#ifndef __HALO_WINDOWS_HOST_UNISTD_H
#define __HALO_WINDOWS_HOST_UNISTD_H

#include <io.h>
#include <process.h>
#include <stdlib.h>
#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long long ssize_t;
#endif

#define _SC_PAGESIZE 1
#define _SC_NPROCESSORS_ONLN 2

int getpagesize(void);
long sysconf(int name);

/* (mkdir: sys/stat.h) */
#include <sys/stat.h>

#endif
