/*
HOST_MEMORY_WIN.C

The guest's memory on Windows: everything it addresses lies below 4 GB, as
on Linux (port/android/host/host_memory.c, whose interface, host.h's, this
implements), since the x32 guest has no base register.

A 64-bit Windows process has nearly all of its low 4 GB free: the
executable, the system's DLLs and the heaps are placed above it (the
executable is linked /DYNAMICBASE /HIGHENTROPYVA, tools/windows_build.py).
So at the top of main, before SDL3.dll and the drivers load, the host
reserves (MEM_RESERVE: address space, no memory)

- the fixed ranges the guest was built for: the Xbox window at 0x80000000
  (HALO_GUEST_WINDOW_SIZE) and the image's range at HALO_GUEST_IMAGE_BASE
  (HALO_GUEST_IMAGE_RESERVE);
- pools for the guest's other memory (its malloc arenas, its threads'
  stacks, its own mappings): POOLS_AT_START of the free address space below
  4 GB, in pieces of at most POOL_SIZE carved from the top of the highest
  free gaps down, and more the same way as they fill. The rest stays free
  for whatever in the process wants low addresses (a driver, say).

If something already holds a fixed range at start-up (host_main_win.c logs
the low map then), host_main_win.c starts the game again as a launcher
does: a suspended copy whose fixed ranges it reserves before the copy's
first instruction, which this file takes over (reserve_fixed_range).

Pages are committed (backed by memory) as the guest maps them, with their
protection, and decommitted as it unmaps them: Windows charges committed
memory to the system's commit limit whatever its protection, so a mapping
the guest makes PROT_NONE (a reservation, in its terms) stays uncommitted.
mprotect commits what it opens of such pages; madvise's "don't need"
decommits and commits again (the pages read as zero, as Linux's do).
*/

#include "host_win.h"
#include "halo_guest_syscall.h"

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE 0x1000ULL
#define GRANULARITY 0x10000ULL
#define LOW_LIMIT 0x100000000ULL
/* (Windows maps nothing below 64 KB) */
#define LOW_START 0x10000ULL
/* a pool's most and least address space; the pools reserved at start-up */
#define POOL_SIZE (256ULL * 1024 * 1024)
#define POOL_MINIMUM (1024ULL * 1024)
#define POOLS_AT_START (1536ULL * 1024 * 1024)
#define MAXIMUM_POOLS 64
/* (Linux's, which halo_guest_syscall.h does not name) */
#define GUEST_ENODEV 19

struct pool
{
	uint64_t base;
	uint64_t pages;
	uint64_t free_pages;
	/* no page below this one is free (where a search starts) */
	uint64_t first_free;
	uint8_t *used; /* 1 for each page handed out */
};

/* A pool is never freed, and its base never changes: pool_count is
published after its entry (with release ordering), so host_low_owns reads
both without the lock, from the exception handlers too. */
static struct pool pools[MAXIMUM_POOLS];
static int pool_count;
static SRWLOCK memory_lock = SRWLOCK_INIT;

static int fixed_reserved;
static int fixed_failed;
static uint64_t window_base, window_end;
static uint64_t image_base, image_end;
/* the end of what the image range holds: the image, then the host's data
there (host_memory_image_extra) */
static uint64_t image_used_end;

/* a bit for each page of the Xbox window the guest has mapped */
static uint8_t window_pages[HALO_GUEST_WINDOW_SIZE / PAGE / 8];

static uint64_t round_up(uint64_t value, uint64_t unit)
{
	return (value + unit - 1) & ~(unit - 1);
}

static int in_range(uint64_t address, uint64_t size, uint64_t base, uint64_t end)
{
	return address >= base && address + size <= end && address + size >= address;
}

static void memory_lock_take(void)
{
	AcquireSRWLockExclusive(&memory_lock);
}

static void memory_lock_give(void)
{
	ReleaseSRWLockExclusive(&memory_lock);
}

/* ---------- the low map */

static const char *state_name(DWORD state)
{
	return state == MEM_FREE ? "free" : state == MEM_RESERVE ? "reserved" : state == MEM_COMMIT ? "committed" : "?";
}

static const char *type_name(DWORD type)
{
	return type == MEM_IMAGE ? "image" : type == MEM_MAPPED ? "mapped" : type == MEM_PRIVATE ? "private" : "";
}

static void log_region(FILE *file, const MEMORY_BASIC_INFORMATION *region)
{
	char line[600];
	wchar_t wide[MAX_PATH];
	char name[MAX_PATH * 3] = "";
	uint64_t start = (uint64_t)(uintptr_t)region->BaseAddress;

	if (region->State != MEM_FREE && (region->Type == MEM_IMAGE || region->Type == MEM_MAPPED) &&
		GetMappedFileNameW(GetCurrentProcess(), region->BaseAddress, wide, MAX_PATH))
	{
		WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, (int)sizeof(name), NULL, NULL);
	}
	snprintf(line, sizeof(line), "low memory %08llx-%08llx %-9s %-7s protect %03lx allocation %08llx %s",
		(unsigned long long)start, (unsigned long long)(start + region->RegionSize), state_name(region->State),
		type_name(region->Type), (unsigned long)region->Protect,
		(unsigned long long)(uintptr_t)region->AllocationBase, name);
	host_log(HOST_LOG_INFO, line);
	if (file)
		fprintf(file, "%s\n", line);
}

void host_memory_log_low_map(FILE *file)
{
	uint64_t address = 0;

	while (address < LOW_LIMIT)
	{
		MEMORY_BASIC_INFORMATION region;

		if (!VirtualQuery((void *)(uintptr_t)address, &region, sizeof(region)) || !region.RegionSize)
			break;
		log_region(file, &region);
		address = (uint64_t)(uintptr_t)region.BaseAddress + region.RegionSize;
	}
}

void host_memory_report_low_mappings(FILE *file)
{
	host_memory_log_low_map(file);
}

/* ---------- start-up */

int host_launched(void)
{
	static int known, launched;

	if (!known)
	{
		known = 1;
		launched = GetEnvironmentVariableW(HOST_LAUNCHED_VARIABLE, NULL, 0) > 0;
		SetEnvironmentVariableW(HOST_LAUNCHED_VARIABLE, NULL);
	}
	return launched;
}

/* whether the region at base is the launcher's reservation of a fixed
range: one allocation of exactly that size, only reserved, with the
protection the launcher gives it (host_main_win.c, launch_reserved) */
static int launcher_reservation(uint64_t base, uint64_t size)
{
	MEMORY_BASIC_INFORMATION region;

	return VirtualQuery((void *)(uintptr_t)base, &region, sizeof(region)) && region.State == MEM_RESERVE &&
		region.Type == MEM_PRIVATE && (uint64_t)(uintptr_t)region.AllocationBase == base &&
		(uint64_t)(uintptr_t)region.BaseAddress == base && region.RegionSize == size &&
		region.AllocationProtect == PAGE_NOACCESS;
}

/* reserves a fixed range; in a copy the launcher started, takes its
reservation of it instead. 0 on success. */
static int reserve_fixed_range(uint64_t base, uint64_t size, const char *what)
{
	DWORD error;

	if (VirtualAlloc((void *)(uintptr_t)base, size, MEM_RESERVE, PAGE_NOACCESS) == (void *)(uintptr_t)base)
		return 0;
	error = GetLastError();
	if (host_launched() && launcher_reservation(base, size))
	{
		host_logf(HOST_LOG_INFO, "the launcher reserved the %s at %08llx", what, (unsigned long long)base);
		return 0;
	}
	host_logf(HOST_LOG_ERROR, "cannot reserve the %s at %08llx-%08llx (error %lu)", what, (unsigned long long)base,
		(unsigned long long)(base + size), (unsigned long)error);
	return -1;
}

/* (memory lock held, or at start-up) a reserved range as a pool; 0 if the
table or the memory for its page map is full */
static int pool_add(uint64_t base, uint64_t size)
{
	struct pool *pool;

	if (pool_count == MAXIMUM_POOLS)
		return 0;
	pool = &pools[pool_count];
	pool->base = base;
	pool->pages = size / PAGE;
	pool->free_pages = pool->pages;
	pool->first_free = 0;
	pool->used = calloc((size_t)pool->pages, 1);
	if (!pool->used)
		return 0;
	__atomic_store_n(&pool_count, pool_count + 1, __ATOMIC_RELEASE);
	return 1;
}

/* the highest free range below 4 GB of at least POOL_MINIMUM bytes, at
whole allocation units (64 KB); 0 if none */
static int highest_gap(uint64_t *start, uint64_t *end)
{
	uint64_t address = LOW_START;
	int found = 0;

	while (address < LOW_LIMIT)
	{
		MEMORY_BASIC_INFORMATION region;
		uint64_t low, high;

		if (!VirtualQuery((void *)(uintptr_t)address, &region, sizeof(region)) || !region.RegionSize)
			break;
		low = round_up((uint64_t)(uintptr_t)region.BaseAddress, GRANULARITY);
		high = (uint64_t)(uintptr_t)region.BaseAddress + region.RegionSize;
		address = high;
		if (high > LOW_LIMIT)
			high = LOW_LIMIT;
		high &= ~(GRANULARITY - 1);
		if (region.State == MEM_FREE && high > low && high - low >= POOL_MINIMUM)
		{
			*start = low;
			*end = high;
			found = 1;
		}
	}
	return found;
}

/* (memory lock held, or at start-up) at least wanted bytes more of pools,
in pieces of at most POOL_SIZE from the top of the highest free gaps; 1 if
any was reserved */
static int pools_grow(uint64_t wanted)
{
	uint64_t total = 0;
	int attempts;

	for (attempts = 0; total < wanted && attempts < 64; attempts++)
	{
		uint64_t start, end, size;

		if (!highest_gap(&start, &end))
			break;
		size = end - start < POOL_SIZE ? end - start : POOL_SIZE;
		start = end - size;
		if (VirtualAlloc((void *)(uintptr_t)start, size, MEM_RESERVE, PAGE_NOACCESS) != (void *)(uintptr_t)start)
			continue;
		if (!pool_add(start, size))
		{
			VirtualFree((void *)(uintptr_t)start, 0, MEM_RELEASE);
			break;
		}
		host_logf(HOST_LOG_INFO, "guest memory pool %d at %08llx-%08llx", pool_count - 1, (unsigned long long)start,
			(unsigned long long)end);
		total += size;
	}
	return total > 0;
}

void host_memory_reserve_early(void)
{
	if (fixed_reserved || fixed_failed)
		return;
	if (reserve_fixed_range(HALO_GUEST_WINDOW_BASE, HALO_GUEST_WINDOW_SIZE, "Xbox memory window") != 0)
	{
		fixed_failed = 1;
		return;
	}
	if (reserve_fixed_range(HALO_GUEST_IMAGE_BASE, HALO_GUEST_IMAGE_RESERVE, "guest image range") != 0)
	{
		VirtualFree((void *)(uintptr_t)HALO_GUEST_WINDOW_BASE, 0, MEM_RELEASE);
		fixed_failed = 1;
		return;
	}
	fixed_reserved = 1;
	window_base = HALO_GUEST_WINDOW_BASE;
	window_end = window_base + HALO_GUEST_WINDOW_SIZE;
	pools_grow(POOLS_AT_START);
}

int host_memory_fixed_reserved(void)
{
	return fixed_reserved;
}

int host_memory_fixed_unavailable(void)
{
	return !fixed_reserved && fixed_failed;
}

int host_memory_initialize(uint32_t base, uint32_t size)
{
	if (base != HALO_GUEST_IMAGE_BASE || size > HALO_GUEST_IMAGE_RESERVE)
	{
		host_logf(HOST_LOG_ERROR, "the guest image (%08x, %u bytes) does not fit its range", base, size);
		return -1;
	}
	host_memory_reserve_early();
	if (!fixed_reserved)
		return -1;
	memory_lock_take();
	image_base = base;
	image_end = base + round_up(size, PAGE);
	image_used_end = image_end;
	memory_lock_give();
	return 0;
}

void *host_memory_image_extra(size_t size)
{
	uint64_t length = round_up(size, PAGE), start;
	void *result = NULL;

	memory_lock_take();
	start = image_used_end;
	if (image_end && start + length <= (uint64_t)HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_RESERVE &&
		VirtualAlloc((void *)(uintptr_t)start, length, MEM_COMMIT, PAGE_READWRITE) == (void *)(uintptr_t)start)
	{
		image_used_end = start + length;
		result = (void *)(uintptr_t)start;
	}
	memory_lock_give();
	return result;
}

/* ---------- committing */

/* commits [address, address + length) with the protection (a POSIX one):
fresh zeroed pages where they were not committed; 0, or -errno */
static long commit(uint64_t address, uint64_t length, int protection)
{
	if (VirtualAlloc((void *)(uintptr_t)address, length, MEM_COMMIT, host_windows_protection(protection)) !=
		(void *)(uintptr_t)address)
	{
		return -host_linux_errno_of_windows(GetLastError());
	}
	/* (committing pages already committed keeps their protection) */
	if (protection != (PROT_READ | PROT_WRITE))
	{
		DWORD previous;

		if (!VirtualProtect((void *)(uintptr_t)address, length, host_windows_protection(protection), &previous))
			return -host_linux_errno_of_windows(GetLastError());
	}
	return 0;
}

static void decommit(uint64_t address, uint64_t length)
{
	VirtualFree((void *)(uintptr_t)address, length, MEM_DECOMMIT);
}

/* maps fresh pages: committed with the protection, or (PROT_NONE) left
reserved */
static long map_fresh(uint64_t address, uint64_t length, int protection)
{
	decommit(address, length);
	if (protection == PROT_NONE)
		return 0;
	return commit(address, length, protection);
}

/* ---------- the pools */

static void *pool_take(struct pool *pool, uint64_t pages)
{
	uint64_t run = 0, page;

	if (pool->free_pages < pages)
		return NULL;
	for (page = pool->first_free; page < pool->pages; page++)
	{
		if (pool->used[page])
		{
			run = 0;
			continue;
		}
		if (++run == pages)
		{
			uint64_t first = page + 1 - pages;

			memset(&pool->used[first], 1, (size_t)pages);
			pool->free_pages -= pages;
			if (first == pool->first_free)
			{
				while (pool->first_free < pool->pages && pool->used[pool->first_free])
					pool->first_free++;
			}
			return (void *)(uintptr_t)(pool->base + first * PAGE);
		}
	}
	return NULL;
}

static struct pool *pool_of(uint64_t address, uint64_t size)
{
	int count = __atomic_load_n(&pool_count, __ATOMIC_ACQUIRE);
	int index;

	for (index = 0; index < count; index++)
	{
		if (in_range(address, size, pools[index].base, pools[index].base + pools[index].pages * PAGE))
			return &pools[index];
	}
	return NULL;
}

/* (memory lock held) the pages go back to their pool, the address space
stays reserved */
static void pool_release(struct pool *pool, uint64_t start, uint64_t length)
{
	uint64_t first = (start - pool->base) / PAGE, count = length / PAGE, page;

	decommit(start, length);
	for (page = first; page < first + count; page++)
	{
		if (pool->used[page])
		{
			pool->used[page] = 0;
			pool->free_pages++;
		}
	}
	if (first < pool->first_free)
		pool->first_free = first;
}

void *host_low_map(size_t size, int protection)
{
	uint64_t pages = round_up(size, PAGE) / PAGE;
	void *address = NULL;
	int index;

	if (!pages || pages > POOL_SIZE / PAGE)
		return NULL;
	memory_lock_take();
	for (index = 0; index < pool_count && !address; index++)
		address = pool_take(&pools[index], pages);
	/* (the pools are full: more of the free address space) */
	if (!address && pools_grow(pages * PAGE))
	{
		for (index = 0; index < pool_count && !address; index++)
			address = pool_take(&pools[index], pages);
	}
	memory_lock_give();
	if (!address)
	{
		host_logf(HOST_LOG_ERROR, "guest memory: no room below 4 GB for %llu bytes", (unsigned long long)(pages * PAGE));
		return NULL;
	}
	if (protection != PROT_NONE && commit((uint64_t)(uintptr_t)address, pages * PAGE, protection) != 0)
	{
		host_low_unmap(address, pages * PAGE);
		return NULL;
	}
	return address;
}

void host_low_unmap(void *address, size_t size)
{
	uint64_t start = (uint64_t)(uintptr_t)address & ~(PAGE - 1);
	uint64_t length = round_up((uint64_t)(uintptr_t)address + size, PAGE) - start;
	struct pool *pool;

	memory_lock_take();
	pool = pool_of(start, length);
	if (pool)
		pool_release(pool, start, length);
	memory_lock_give();
}

int host_low_owns(uintptr_t address, size_t size)
{
	if (in_range(address, size, window_base, window_end) || in_range(address, size, image_base, image_end))
		return 1;
	return pool_of(address, size) != NULL;
}

/* ---------- the guest's memory system calls

As on Linux (host_memory.c): the guest maps, unmaps, reprotects and advises
only memory that is its own (the pools' pages, the Xbox window, the image's
range); a "no replace" mapping fails where something is mapped. What each
page is in use for: a pool's used[], window_pages for the window. All under
the memory lock. */

enum
{
	PAGE_HOST, /* the host's, or address space no one reserved */
	PAGE_POOL,
	PAGE_WINDOW,
	PAGE_IMAGE,
};

static int window_page(uint64_t address)
{
	uint64_t page = (address - window_base) / PAGE;

	return (window_pages[page >> 3] >> (page & 7)) & 1;
}

static void set_window_pages(uint64_t address, uint64_t length, int value)
{
	uint64_t page;

	for (page = (address - window_base) / PAGE; page < (address + length - window_base) / PAGE; page++)
	{
		if (value)
			window_pages[page >> 3] |= (uint8_t)(1 << (page & 7));
		else
			window_pages[page >> 3] &= (uint8_t)~(1 << (page & 7));
	}
}

static int page_kind(uint64_t address, struct pool **pool)
{
	*pool = NULL;
	if (in_range(address, PAGE, window_base, window_end))
		return PAGE_WINDOW;
	if (in_range(address, PAGE, image_base, image_end))
		return PAGE_IMAGE;
	*pool = pool_of(address, PAGE);
	return *pool ? PAGE_POOL : PAGE_HOST;
}

/* the end of the run of pages from address (before end) of the same kind,
and pool, as the first */
static uint64_t run_end(uint64_t address, uint64_t end, int *kind, struct pool **pool)
{
	struct pool *next_pool;

	*kind = page_kind(address, pool);
	for (address += PAGE; address < end; address += PAGE)
	{
		if (page_kind(address, &next_pool) != *kind || next_pool != *pool)
			break;
	}
	return address;
}

static int run_in_use(uint64_t address, uint64_t end, int kind, const struct pool *pool)
{
	for (; address < end; address += PAGE)
	{
		if (kind == PAGE_IMAGE)
			return 1;
		if (kind == PAGE_POOL && pool->used[(address - pool->base) / PAGE])
			return 1;
		if (kind == PAGE_WINDOW && window_page(address))
			return 1;
	}
	return 0;
}

/* whether every page of a range is the guest's (PAGE_IMAGE too, unless
image is 0) */
static int guest_owns(uint64_t address, uint64_t end, int image)
{
	while (address < end)
	{
		struct pool *pool;
		int kind;

		address = run_end(address, end, &kind, &pool);
		if (kind == PAGE_HOST || (kind == PAGE_IMAGE && !image))
			return 0;
	}
	return 1;
}

static void forget_window_range(uint64_t address, uint64_t length)
{
	uint64_t start = address, end = address + length;

	if (start < window_base)
		start = window_base;
	if (end > window_end)
		end = window_end;
	if (start < end)
		host_memory_watch_forget((uint32_t)start, (uint32_t)(end - start));
}

/* (memory lock held) whether any page of a range is the host's, and
whether any of the guest's is in use */
static void range_kinds(uint64_t address, uint64_t end, int *host, int *owned, int *in_use)
{
	*host = *owned = *in_use = 0;
	while (address < end)
	{
		struct pool *pool;
		int kind;
		uint64_t next = run_end(address, end, &kind, &pool);

		if (kind == PAGE_HOST)
			*host = 1;
		else
		{
			*owned = 1;
			*in_use |= run_in_use(address, next, kind, pool);
		}
		address = next;
	}
}

static long guest_mmap_fixed(uint64_t address, uint64_t length, int protection, int flags)
{
	uint64_t end = address + length, at;
	int replace = !(flags & GUEST_MAP_FIXED_NOREPLACE);
	int host, owned, in_use;
	long status;

	if (address & (PAGE - 1))
		return -GUEST_EINVAL;
	if (end > LOW_LIMIT || end < address)
		return -GUEST_ENOMEM;
	memory_lock_take();
	range_kinds(address, end, &host, &owned, &in_use);
	if (host && !owned && !replace)
	{
		/* free address space: a range of the guest's own where nothing is
		(the Custom Edition tag cache, xbox_memory.c), reserved as a pool
		of its own, in whole allocation units */
		uint64_t start = address & ~(GRANULARITY - 1);
		uint64_t stop = round_up(end, GRANULARITY);

		if (VirtualAlloc((void *)(uintptr_t)start, stop - start, MEM_RESERVE, PAGE_NOACCESS) ==
			(void *)(uintptr_t)start)
		{
			if (pool_add(start, stop - start))
				range_kinds(address, end, &host, &owned, &in_use);
			else
				VirtualFree((void *)(uintptr_t)start, 0, MEM_RELEASE);
		}
	}
	if (host)
	{
		/* (address space that is not the guest's: never replaced) */
		status = replace ? -GUEST_EINVAL : -GUEST_EEXIST;
		goto done;
	}
	if (!replace && in_use)
	{
		status = -GUEST_EEXIST;
		goto done;
	}
	status = map_fresh(address, length, protection);
	if (status)
		goto done;
	for (at = address; at < end;)
	{
		struct pool *pool;
		int kind;
		uint64_t next = run_end(at, end, &kind, &pool);
		uint64_t page;

		if (kind == PAGE_POOL)
		{
			for (page = (at - pool->base) / PAGE; page < (next - pool->base) / PAGE; page++)
			{
				if (!pool->used[page])
				{
					pool->used[page] = 1;
					pool->free_pages--;
				}
			}
		}
		else if (kind == PAGE_WINDOW)
		{
			set_window_pages(at, next - at, 1);
		}
		at = next;
	}
	forget_window_range(address, length);
	status = (long)address;
done:
	memory_lock_give();
	return status;
}

long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset)
{
	uint64_t length = round_up(size, PAGE);
	void *result;

	(void)offset;
	if (!length || length < size)
		return -GUEST_EINVAL;
	if (fd != -1 || !(flags & GUEST_MAP_ANONYMOUS))
	{
		static int logged;

		if (!logged)
		{
			logged = 1;
			host_logf(HOST_LOG_WARN, "the guest mapped a file (descriptor %d), which this host does not do", fd);
		}
		return -GUEST_ENODEV;
	}
	if (flags & (GUEST_MAP_FIXED | GUEST_MAP_FIXED_NOREPLACE))
		return guest_mmap_fixed(address, length, protection, flags);
	result = host_low_map(length, protection);
	return result ? (long)(uintptr_t)result : -GUEST_ENOMEM;
}

long host_guest_munmap(uint64_t address, uint64_t size)
{
	uint64_t length = round_up(size, PAGE), end = address + length, at;
	long status = 0;

	if ((address & (PAGE - 1)) || !length || length < size)
		return -GUEST_EINVAL;
	if (end > LOW_LIMIT || end < address)
		return -GUEST_EINVAL;
	memory_lock_take();
	if (!guest_owns(address, end, 0))
	{
		status = -GUEST_EINVAL;
		goto done;
	}
	for (at = address; at < end;)
	{
		struct pool *pool;
		int kind;
		uint64_t next = run_end(at, end, &kind, &pool);

		if (kind == PAGE_POOL)
			pool_release(pool, at, next - at);
		else if (kind == PAGE_WINDOW)
		{
			decommit(at, next - at);
			set_window_pages(at, next - at, 0);
		}
		at = next;
	}
	forget_window_range(address, length);
done:
	memory_lock_give();
	return status;
}

/* the range, page-aligned, if all of it is the guest's: 1, 0 for an empty
one, or -errno */
static long guest_range(uint64_t address, uint64_t size, uint64_t *length)
{
	int owned;

	if (address & (PAGE - 1))
		return -GUEST_EINVAL;
	*length = round_up(size, PAGE);
	if (*length < size)
		return -GUEST_ENOMEM;
	if (!*length)
		return 0;
	if (address + *length > LOW_LIMIT || address + *length < address)
		return -GUEST_ENOMEM;
	memory_lock_take();
	owned = guest_owns(address, address + *length, 1);
	memory_lock_give();
	return owned ? 1 : -GUEST_ENOMEM;
}

/* calls function on each run of [address, address + length) that is in one
state (MEM_COMMIT or MEM_RESERVE), with its protection; the first failure's
-errno, or 0 */
static long each_region(uint64_t address, uint64_t length,
	long (*function)(uint64_t start, uint64_t size, DWORD state, DWORD protect, int argument), int argument)
{
	uint64_t end = address + length;

	while (address < end)
	{
		MEMORY_BASIC_INFORMATION region;
		uint64_t region_end;
		long status;

		if (!VirtualQuery((void *)(uintptr_t)address, &region, sizeof(region)))
			return -GUEST_ENOMEM;
		region_end = (uint64_t)(uintptr_t)region.BaseAddress + region.RegionSize;
		if (region_end > end)
			region_end = end;
		status = function(address, region_end - address, region.State, region.Protect, argument);
		if (status)
			return status;
		address = region_end;
	}
	return 0;
}

static long protect_region(uint64_t start, uint64_t size, DWORD state, DWORD protect, int protection)
{
	DWORD previous;

	(void)protect;
	if (state == MEM_COMMIT)
	{
		if (!VirtualProtect((void *)(uintptr_t)start, size, host_windows_protection(protection), &previous))
			return -host_linux_errno_of_windows(GetLastError());
		return 0;
	}
	/* (a page mapped PROT_NONE, never committed, opened: zeroed) */
	return protection == PROT_NONE ? 0 : commit(start, size, protection);
}

long host_guest_mprotect(uint64_t address, uint64_t size, int protection)
{
	uint64_t length;
	long check = guest_range(address, size, &length);
	long status;

	if (check <= 0)
		return check;
	memory_lock_take();
	status = each_region(address, length, protect_region, protection);
	memory_lock_give();
	if (status)
		return status;
	forget_window_range(address, length);
	return 0;
}

static long advise_region(uint64_t start, uint64_t size, DWORD state, DWORD protect, int advice)
{
	if (state != MEM_COMMIT)
		return 0;
	if (advice == GUEST_MADV_DONTNEED)
	{
		/* (the pages read as zero again, as the guest may count on) */
		decommit(start, size);
		if (VirtualAlloc((void *)(uintptr_t)start, size, MEM_COMMIT, protect) != (void *)(uintptr_t)start)
			return -host_linux_errno_of_windows(GetLastError());
		return 0;
	}
	/* MADV_FREE: their contents may go, or stay */
	VirtualAlloc((void *)(uintptr_t)start, size, MEM_RESET, PAGE_NOACCESS);
	return 0;
}

long host_guest_madvise(uint64_t address, uint64_t size, int advice)
{
	uint64_t length;
	long check = guest_range(address, size, &length);
	long status;

	if (check <= 0)
		return check;
	if (advice != GUEST_MADV_DONTNEED && advice != GUEST_MADV_FREE)
		return 0;
	memory_lock_take();
	status = each_region(address, length, advise_region, advice);
	memory_lock_give();
	if (status)
		return status;
	forget_window_range(address, length);
	return 0;
}

size_t host_page_size(void)
{
	return PAGE;
}

/* (host_memory_watch.c) the window's pages are the guest's own, with the
protection it gave them */
int host_memory_window_protection(uintptr_t address)
{
	(void)address;
	return PROT_READ | PROT_WRITE;
}
