/*
HOST.H

Internals of the Android port's host library (libmain.so). See
port/android/README.md for the overall design and
port/android/include/halo_android_abi.h for the guest contract.
*/

#ifndef __HALO_ANDROID_HOST_H
#define __HALO_ANDROID_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "halo_android_abi.h"

/* ---------- logging (logcat tag "halo") */

void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6
/* guest services also used inside the host (host_main.c) */
void host_exit(int code) __attribute__((noreturn));
void host_abort(const char *reason) __attribute__((noreturn));
int host_errno(void);

/* logs, shows the message to the player and terminates */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

/* ---------- the guest's addresses

A guest address is 32 bits. Without the base register it is the host
address itself: the guest's memory lies in the low 4 GB of the process.
With it (HALO_GUEST_BASE: tools/guest_base_register.py, for macOS, whose
processes have no low 4 GB, and as an option on Linux and Android), the
guest's 4 GB lie at host_guest_base, a multiple of 4 GB the host chooses
(host_memory_base.c), and a guest address is the offset from it: the low
32 bits of the host address. */

#ifdef HALO_GUEST_BASE
extern uintptr_t host_guest_base;
#else
#define host_guest_base ((uintptr_t)0)
#endif

/* the host address of a guest address, 0 included */
static inline uintptr_t host_address(uint32_t address)
{
	return host_guest_base + address;
}

/* a guest pointer as the host's (NULL for 0) */
static inline void *host_pointer(uint32_t address)
{
	return address ? (void *)host_address(address) : NULL;
}

/* the guest address of a host address in the guest's memory */
static inline uint32_t guest_address(const void *pointer)
{
	return (uint32_t)(uintptr_t)pointer;
}

/* whether a host address lies in the guest's memory */
static inline int host_in_guest(uintptr_t address)
{
	return address - host_guest_base < 0x100000000ULL;
}

/* ---------- guest memory (host_memory.c; host_memory_base.c with the base
register)

All memory the guest can address lies in its 4 GB. The host reserves the
Xbox window and the image's range at start-up, and hands out pages for
everything else (the guest's malloc arenas, thread stacks, anonymous
mappings) from pools of address space it reserves there on demand. The
functions that take or return a pointer take or return host addresses. */

/* reserves the fixed ranges (the Xbox window and the image's) as early as
the process allows: from JNI_OnLoad, before the Java side of the game's
process allocates much (host_main.c); a failure is reported again by
host_memory_initialize */
void host_memory_reserve_early(void);
/* takes the fixed ranges for the image; returns 0 on success */
int host_memory_initialize(uint32_t image_base, uint32_t image_size);
/* 1 if the fixed ranges could not be reserved (something else holds them) */
int host_memory_fixed_unavailable(void);
/* lists the mappings below 4 GB in logcat and, if file is not NULL, in it */
void host_memory_report_low_mappings(FILE *file);
/* Makes the write tracking compare page contents instead of catching page
faults (host_watch_hash.h), for when the app cannot receive its own
SIGSEGV (ARM translation). Call before the guest starts. */
void host_memory_watch_use_hashes(void);
/* the host's page size (4 KB on Linux and Android without the base
register, which needs it: the Xbox window's pages; 16 KB on macOS) */
size_t host_page_size(void);
/* page-granular allocations in the guest's memory; NULL on failure */
void *host_low_map(size_t size, int protection);
void host_low_unmap(void *address, size_t size);
/* 1 if [address, address + size) was handed out by host_low_map or is one of
the fixed ranges */
int host_low_owns(uintptr_t address, size_t size);
/* the guest's mmap/munmap/mprotect (host_syscall.c), with guest addresses
and Linux's flags; mmap returns the guest address or -errno */
long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset);
long host_guest_munmap(uint64_t address, uint64_t size);
long host_guest_mprotect(uint64_t address, uint64_t size, int protection);

/* ---------- the guest image (host_loader.c) */

struct host_guest_image
{
	const struct halo_guest_header *header;
	uint32_t base, end;
};

extern struct host_guest_image host_image;

/* maps the image from the ELF file in memory; returns 0 on success */
int host_load_image(const void *elf, size_t size);

/* the signal handlers that report crashes and catch the write tracking's
faults (host_memory_watch.c) */
void host_install_signal_handlers(void);
/* (host_memory_watch.c) a write to a write-protected page of the Xbox
window, at that host address: the tracking unprotects it; 1 if it was one */
int host_memory_watch_fault(uintptr_t address);
/* memory_watch_forget (host_memory_watch.c): the pages of a range of the
window (guest addresses) count as written and are no longer watched */
void host_memory_watch_forget(uint32_t address, uint32_t size);
/* (host_memory.c or host_memory_base.c) the protection the guest asked for
of the Xbox window's host page at that address, which the write tracking
puts back */
int host_memory_window_protection(uintptr_t address);

/* ---------- entering guest code (host_thread.c) */

/* calls the guest function at address with up to four 32-bit arguments on
this thread, which must have been made by host_native_thread_create (giving
the thread a guest struct pthread first if it has none); returns the
guest's w0 */
uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
#ifdef HALO_GUEST_BASE
/* host_enter.S: calls the guest code at the host address function with x21
set to base and the four arguments zero-extended; returns its w0 */
uint32_t host_enter_guest(uintptr_t function, uintptr_t base, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
/* host_enter.S: calls function(argument) on the stack whose top is
stack_top (for a thread whose own stack is outside the guest's memory) */
void host_run_on_stack(void (*function)(void *), void *argument, void *stack_top);
#endif
/* starts a thread running function(argument) with its stack in guest
memory, so that it can call guest code; the stack is freed after it exits.
Returns 0 or an errno value */
int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size);
/* runs the guest's __guest_start on the calling thread (one made by
host_native_thread_create); does not return */
void host_run_guest_main(uint32_t boot) __attribute__((noreturn));

/* ---------- debugging (host_debug.c) */

void host_debug_thread_started(void);
void host_debug_thread_exited(void);
/* config.toml's debug.sample_seconds: seconds between samples of the guest
threads, as text */
void host_debug_start_sampler(const char *setting);

/* ---------- import table (host_imports.c) */

/* the host function for an import name, or NULL */
void *host_resolve_import(const char *name);

/* ---------- SDL / GL (host_sdl.c, host_gl.c) */

/* SDL objects are 64-bit pointers, which the guest cannot hold: it gets
small handles into a table instead (host_sdl.c; port/linux/arm64's
host_desktop.c uses it too) */
enum host_sdl_handle_type
{
	_handle_free,
	_handle_window,
	_handle_context,
	_handle_gamepad,
	_handle_audio,
	_handle_renderer,
};

/* the object's handle (the same one again for an object that has one), or
0 for NULL or a full table */
uint32_t host_sdl_handle_new(int type, void *object);
/* the object of a handle of that type, or NULL */
void *host_sdl_handle_get(uint32_t handle, int type);
void host_sdl_handle_free(uint32_t handle);

void *host_gl_resolve(const char *name);

#endif
