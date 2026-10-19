/*
SYS/STAT.H

The C runtime's <sys/stat.h>, and POSIX's mkdir(path, mode) for the 64-bit
Windows host's shared files: the C runtime's takes no mode, and this one a
UTF-8 path (port/windows/host/host_posix_win.c).
*/

#ifndef __HALO_WINDOWS_HOST_SYS_STAT_H
#define __HALO_WINDOWS_HOST_SYS_STAT_H

#include_next <sys/stat.h>
/* (the C runtime's own mkdir, declared first: a file that includes
<direct.h> after this one would otherwise have its declaration taken for a
call of the macro below) */
#include <direct.h>

int halo_windows_mkdir(const char *path, int mode);
#define mkdir(path, mode) halo_windows_mkdir(path, mode)

#endif
