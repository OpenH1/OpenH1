/*
SYS/MMAN.H

The memory mapping the 64-bit Windows host's shared files use
(port/android/host/host_loader.c, host_memory_watch.c), over VirtualAlloc
and VirtualProtect (port/windows/host/host_posix_win.c): anonymous memory
only. A MAP_FIXED mapping commits pages of address space the host reserved
(the guest's, host_memory_win.c); any other is an allocation of its own,
which munmap releases whole.
*/

#ifndef __HALO_WINDOWS_HOST_SYS_MMAN_H
#define __HALO_WINDOWS_HOST_SYS_MMAN_H

#include <stddef.h>

/* (Linux's values, which are the guest's too: halo_guest_syscall.h) */
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_FIXED_NOREPLACE 0x100000

#define MAP_FAILED ((void *)-1)

void *mmap(void *address, size_t length, int protection, int flags, int descriptor, long long offset);
int munmap(void *address, size_t length);
int mprotect(void *address, size_t length, int protection);
/* 0 if every page of the range is committed and readable (the Vulkan
renderer's check before it reads guest memory), each residency byte 1;
else -1 with errno ENOMEM */
int mincore(void *address, size_t length, unsigned char *residency);

#endif
