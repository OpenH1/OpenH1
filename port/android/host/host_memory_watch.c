/*
HOST_MEMORY_WATCH.C

Guest memory write tracking (the interface of port/linux/src/memory_watch.c)
and the host's fault handlers.

The renderer write-protects the pages behind the textures it caches, and
the fault handler here records the first write to each. Other faults are
reported (with guest-relative addresses) and passed on to the previous
handler. Under ARM translation (the x86 emulator) page faults cannot be
caught, and the tracking compares page contents instead
(host_watch_hash.c).

The guest names its ranges with guest addresses (host.h); the pages
protected are the host's (4 KB on Linux and Android, 16 KB on macOS), so on
a host with larger pages a write marks a little more than it touched.
*/

#include "host.h"
#include "host_watch_hash.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#include <unistd.h>

/* the registers in a signal's context */
#ifdef __APPLE__
#define CONTEXT_PC(context) ((ucontext_t *)(context))->uc_mcontext->__ss.__pc
#define CONTEXT_SP(context) ((ucontext_t *)(context))->uc_mcontext->__ss.__sp
#define CONTEXT_FP(context) ((ucontext_t *)(context))->uc_mcontext->__ss.__fp
#define CONTEXT_LR(context) ((ucontext_t *)(context))->uc_mcontext->__ss.__lr
/* x0-x28, then fp and lr */
static uint64_t context_register(void *context, int index)
{
	return index < 29 ? ((ucontext_t *)context)->uc_mcontext->__ss.__x[index] :
		index == 29 ? CONTEXT_FP(context) : CONTEXT_LR(context);
}
#else
#define CONTEXT_REGISTERS(context) ((struct sigcontext *)&((ucontext_t *)(context))->uc_mcontext)
#define CONTEXT_PC(context) CONTEXT_REGISTERS(context)->pc
#define CONTEXT_SP(context) CONTEXT_REGISTERS(context)->sp
#define CONTEXT_FP(context) CONTEXT_REGISTERS(context)->regs[29]
#define CONTEXT_LR(context) CONTEXT_REGISTERS(context)->regs[30]
static uint64_t context_register(void *context, int index)
{
	return CONTEXT_REGISTERS(context)->regs[index];
}
#endif

/* the tracking's pages: the host's under page protection; the hashing's
(WATCH_HASH_PAGE_SIZE) otherwise */
#define WATCH_PAGE_COUNT (HALO_GUEST_WINDOW_SIZE / WATCH_HASH_PAGE_SIZE)

static uint8_t page_protected[WATCH_PAGE_COUNT];
static uint32_t page_generation[WATCH_PAGE_COUNT];
static volatile uint32_t current_generation = 1;
static int watch_active;
static int watch_hashing;
static uint8_t page_hash_watched[WATCH_PAGE_COUNT];
static uint64_t page_hash[WATCH_PAGE_COUNT];
static uint32_t page_hashed_frame[WATCH_PAGE_COUNT];
static struct watch_hash page_hashes =
{
	.page_count = WATCH_PAGE_COUNT,
	.watched = page_hash_watched,
	.hash = page_hash,
	.hashed_frame = page_hashed_frame,
	.generation = page_generation,
	.current_generation = &current_generation,
	.frame = 1,
};

/* (the window's host address, once the guest's base is known) */
static struct watch_hash *hashes(void)
{
	if (!page_hashes.base)
		page_hashes.base = (const uint8_t *)host_address(HALO_GUEST_WINDOW_BASE);
	return &page_hashes;
}

static uint64_t watch_page_size(void)
{
	return watch_hashing ? WATCH_HASH_PAGE_SIZE : host_page_size();
}

static uint64_t watch_page_count(void)
{
	return HALO_GUEST_WINDOW_SIZE / watch_page_size();
}

static int in_window(uint64_t address)
{
	return address >= HALO_GUEST_WINDOW_BASE && address - HALO_GUEST_WINDOW_BASE < HALO_GUEST_WINDOW_SIZE;
}

static uint64_t watch_page(uint64_t address)
{
	return (address - HALO_GUEST_WINDOW_BASE) / watch_page_size();
}

/* the host address of a page under page protection */
static void *page_address(uint64_t page)
{
	return (void *)host_address((uint32_t)(HALO_GUEST_WINDOW_BASE + page * host_page_size()));
}

static void mark_written(uint64_t page)
{
	void *address = page_address(page);

	page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	page_protected[page] = 0;
	mprotect(address, host_page_size(), host_memory_window_protection((uintptr_t)address));
}

int host_memory_watch_fault(uintptr_t address)
{
	uint64_t guest;

	if (!watch_active || watch_hashing || !host_in_guest(address))
		return 0;
	guest = address - host_guest_base;
	if (!in_window(guest) || !page_protected[watch_page(guest)])
		return 0;
	mark_written(watch_page(guest));
	return 1;
}

static struct sigaction previous_segv, previous_bus, previous_ill;

static void report_crash(int signal_number, siginfo_t *information, void *context)
{
	uint64_t pc = CONTEXT_PC(context), lr = CONTEXT_LR(context);
	int index;

	host_logf(HOST_LOG_ERROR, "signal %d at address %p: pc %016llx lr %016llx sp %016llx",
		signal_number, information->si_addr, (unsigned long long)pc, (unsigned long long)lr,
		(unsigned long long)CONTEXT_SP(context));
	if (host_guest_base)
		host_logf(HOST_LOG_ERROR, "  guest memory at %016llx", (unsigned long long)host_guest_base);
	if (host_in_guest(pc) && (uint32_t)pc >= host_image.base && (uint32_t)pc < host_image.end)
		host_logf(HOST_LOG_ERROR, "  in the guest image: addr2line -e halo_guest.elf 0x%llx 0x%llx",
			(unsigned long long)(uint32_t)pc, (unsigned long long)(uint32_t)lr);
	for (index = 0; index < 31; index += 4)
	{
		host_logf(HOST_LOG_ERROR, "  x%-2d %016llx %016llx %016llx %016llx", index,
			(unsigned long long)context_register(context, index),
			(unsigned long long)(index + 1 < 31 ? context_register(context, index + 1) : 0),
			(unsigned long long)(index + 2 < 31 ? context_register(context, index + 2) : 0),
			(unsigned long long)(index + 3 < 31 ? context_register(context, index + 3) : 0));
	}
	/* the guest's frame records: fp and lr, 8 bytes each (guest addresses
	in the low 32 bits of the return addresses) */
	{
		uint64_t fp = CONTEXT_FP(context);

		for (index = 0; index < 24 && fp && host_in_guest(fp) && (fp & 7) == 0; index++)
		{
			const uint64_t *frame = (const uint64_t *)fp;

			if (!host_low_owns(fp, 16))
				break;
			host_logf(HOST_LOG_ERROR, "  frame %2d: return %016llx", index, (unsigned long long)frame[1]);
			fp = frame[0];
		}
	}
}

static void chain(struct sigaction *previous, int signal_number, siginfo_t *information, void *context)
{
	sigaction(signal_number, previous, NULL);
	if (previous->sa_flags & SA_SIGINFO)
	{
		if (previous->sa_sigaction)
			previous->sa_sigaction(signal_number, information, context);
	}
	else if (previous->sa_handler != SIG_DFL && previous->sa_handler != SIG_IGN)
	{
		previous->sa_handler(signal_number);
	}
	/* returning re-executes the faulting instruction under the previous
	(or default) handler */
}

static void segv_handler(int signal_number, siginfo_t *information, void *context)
{
	if (host_memory_watch_fault((uintptr_t)information->si_addr))
		return;
	report_crash(signal_number, information, context);
	chain(&previous_segv, signal_number, information, context);
}

static void bus_handler(int signal_number, siginfo_t *information, void *context)
{
#ifdef __APPLE__
	/* (macOS raises SIGBUS for a write to a read-only page) */
	if (host_memory_watch_fault((uintptr_t)information->si_addr))
		return;
#endif
	report_crash(signal_number, information, context);
	chain(&previous_bus, signal_number, information, context);
}

static void ill_handler(int signal_number, siginfo_t *information, void *context)
{
	report_crash(signal_number, information, context);
	chain(&previous_ill, signal_number, information, context);
}

void host_install_signal_handlers(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	action.sa_sigaction = segv_handler;
	sigaction(SIGSEGV, &action, &previous_segv);
	action.sa_sigaction = bus_handler;
	sigaction(SIGBUS, &action, &previous_bus);
	action.sa_sigaction = ill_handler;
	sigaction(SIGILL, &action, &previous_ill);
}

void host_memory_watch_initialize(void)
{
	watch_active = 1;
}

void host_memory_watch_use_hashes(void)
{
	watch_hashing = 1;
}

/* the first and last tracked page of a range of the window, guest
addresses; 0 if none */
static int watch_range(uint32_t address, uint32_t size, uint64_t *first, uint64_t *last)
{
	if (!size || !in_window(address))
		return 0;
	*first = watch_page(address);
	*last = watch_page((uint64_t)address + size - 1);
	if (*last >= watch_page_count())
		*last = watch_page_count() - 1;
	return 1;
}

/* memory_watch_protect: write-protects the pages of a range, or keeps their
hash under host_memory_watch_use_hashes; a size outside the guest window
is ignored */
void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!watch_active || !watch_range(address, size, &first, &last))
		return;
	if (watch_hashing)
	{
		watch_hash_protect(hashes(), (uint32_t)first, (uint32_t)last);
		return;
	}
	for (page = first; page <= last; page++)
	{
		if (!page_protected[page])
		{
			page_protected[page] = 1;
			mprotect(page_address(page), host_page_size(), PROT_READ);
		}
	}
}

uint32_t host_memory_watch_serial(void)
{
	return current_generation;
}

/* memory_watch_generation: the newest generation of the pages of a range.
Under hashing this hashes the pages again, once a frame. */
uint32_t host_memory_watch_generation(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;
	uint32_t newest = 0;

	if (!watch_range(address, size, &first, &last))
		return 0;
	if (watch_hashing)
		return watch_hash_generation(hashes(), (uint32_t)first, (uint32_t)last);
	for (page = first; page <= last; page++)
	{
		if (page_generation[page] > newest)
			newest = page_generation[page];
	}
	return newest;
}

/* memory_watch_prepare_write: unprotects the pages the host itself is about
to write into. Nothing under hashing: the write shows in the next hash and
there is no protection to fault on. */
void host_memory_watch_prepare_write(uint32_t address, uint32_t size)
{
	uint64_t start = address, first, last, page;

	if (!watch_active || watch_hashing || !size)
		return;
	if (start + size <= HALO_GUEST_WINDOW_BASE || start >= (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
		return;
	if (start < HALO_GUEST_WINDOW_BASE)
		start = HALO_GUEST_WINDOW_BASE;
	first = watch_page(start);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= watch_page_count())
		last = watch_page_count() - 1;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page])
			mark_written(page);
	}
}

/* memory_watch_forget: the pages of a range were remapped or reprotected, so
treat them as written and unwatched */
void host_memory_watch_forget(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!watch_range(address, size, &first, &last))
		return;
	if (watch_hashing)
	{
		watch_hash_forget(hashes(), (uint32_t)first, (uint32_t)last);
		return;
	}
	for (page = first; page <= last; page++)
	{
		page_protected[page] = 0;
		page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	}
}

/* memory_watch_begin_frame: pages may be hashed again; nothing under page
protection */
void host_memory_watch_begin_frame(void)
{
	if (watch_hashing)
		watch_hash_begin_frame(hashes());
}
