/*
HARNESS.C

Runs a kernel of bench.c and times it (tools/base_register_bench/run.py):

- harness image.elf base KERNEL SCALE REPEATS: the guest image built with the
  base register, its 4 GB anywhere (as on macOS: port/android/host's
  host_memory_base.c and host_enter.S);
- harness image.elf low KERNEL SCALE REPEATS: the guest image built without
  it, in the low 4 GB (as on Linux and Android: Linux only);
- with -DBENCH_NATIVE, bench.c is linked in and run as native code:
  harness native KERNEL SCALE REPEATS.

The guest runs on a stack in its own memory, as the game's threads do.
Prints one line: the kernel's checksum, the best time in nanoseconds and,
on macOS, the instructions and cycles of that run.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <sys/resource.h>
#endif

/* port/android/host/host_enter.S */
uint32_t host_enter_guest(uintptr_t function, uintptr_t base, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
void host_run_on_stack(void (*function)(void *), void *argument, void *stack_top);

#ifdef BENCH_NATIVE
uint32_t bench_run(uint32_t kernel, uint32_t scale);
#endif

#define GUEST_SIZE 0x100000000ULL
#define STACK_ADDRESS 0x10000000ULL
#define STACK_SIZE 0x100000ULL

struct elf_header
{
	unsigned char ident[16];
	uint16_t type, machine;
	uint32_t version;
	uint64_t entry, program_headers, section_headers;
	uint32_t flags;
	uint16_t header_size, program_header_size, program_header_count, section_header_size, section_header_count,
		section_names;
};

struct elf_program_header
{
	uint32_t type, flags;
	uint64_t offset, address, physical, file_size, memory_size, align;
};

static uintptr_t base;
static uint64_t entry;

static struct
{
	uint32_t kernel, scale, result;
	int native;
} call;

static void run_call(void *unused)
{
	(void)unused;
#ifdef BENCH_NATIVE
	call.result = bench_run(call.kernel, call.scale);
#else
	if (base)
		call.result = host_enter_guest(base + entry, base, call.kernel, call.scale, 0, 0);
	else
		call.result = ((uint32_t (*)(uint32_t, uint32_t))(uintptr_t)entry)(call.kernel, call.scale);
#endif
}

static uint64_t now(void)
{
	struct timespec value;

	clock_gettime(CLOCK_MONOTONIC, &value);
	return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
}

static void counters(uint64_t *instructions, uint64_t *cycles)
{
#ifdef __APPLE__
	struct rusage_info_v4 information;

	if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&information) == 0)
	{
		*instructions = information.ri_instructions;
		*cycles = information.ri_cycles;
		return;
	}
#endif
	*instructions = *cycles = 0;
}

/* maps a page-aligned range at address: fixed, over a reservation */
static void *map_at(uintptr_t address, uint64_t size, int protection)
{
	void *result = mmap((void *)address, size, protection, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);

	if (result != (void *)address)
	{
		fprintf(stderr, "cannot map %llx\n", (unsigned long long)address);
		exit(1);
	}
	return result;
}

#ifndef BENCH_NATIVE
static void load(const char *path, int with_base)
{
	FILE *file = fopen(path, "rb");
	long size;
	unsigned char *image;
	const struct elf_header *header;
	uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
	int index;

	if (!file)
	{
		perror(path);
		exit(1);
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	image = malloc((size_t)size);
	if (fread(image, 1, (size_t)size, file) != (size_t)size)
		exit(1);
	fclose(file);
	header = (const struct elf_header *)image;
	if (with_base)
	{
		/* the 4 GB, aligned to 4 GB, from a reservation of twice that */
		char *reservation = mmap(NULL, 2 * GUEST_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);

		if (reservation == MAP_FAILED)
		{
			perror("mmap");
			exit(1);
		}
		base = ((uintptr_t)reservation + GUEST_SIZE - 1) & ~(uintptr_t)(GUEST_SIZE - 1);
	}
	for (index = 0; index < header->program_header_count; index++)
	{
		const struct elf_program_header *segment = (const struct elf_program_header *)
			(image + header->program_headers + (uint64_t)index * header->program_header_size);
		uint64_t start = segment->address & ~(page - 1);
		uint64_t end = (segment->address + segment->memory_size + page - 1) & ~(page - 1);

		if (segment->type != 1)
			continue;
		map_at(base + start, end - start, PROT_READ | PROT_WRITE);
		memcpy((void *)(base + segment->address), image + segment->offset, segment->file_size);
		if (segment->flags & 1)
		{
			mprotect((void *)(base + start), end - start, PROT_READ | PROT_EXEC);
			__builtin___clear_cache((char *)(base + start), (char *)(base + end));
		}
	}
	entry = header->entry;
	free(image);
}
#endif

int main(int argc, char **argv)
{
	uint64_t best = ~0ULL, best_instructions = 0, best_cycles = 0;
	int repeats, repeat;
	char *stack;

	if (argc != 6 && !(argc == 5 && !strcmp(argv[1], "native")))
	{
		fprintf(stderr, "usage: harness image.elf base|low KERNEL SCALE REPEATS\n"
			"       harness native KERNEL SCALE REPEATS\n");
		return 2;
	}
#ifdef BENCH_NATIVE
	call.kernel = (uint32_t)atoi(argv[2]);
	call.scale = (uint32_t)atoi(argv[3]);
	repeats = atoi(argv[4]);
	stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
#else
	load(argv[1], !strcmp(argv[2], "base"));
	call.kernel = (uint32_t)atoi(argv[3]);
	call.scale = (uint32_t)atoi(argv[4]);
	repeats = atoi(argv[5]);
	/* the stack in the guest's memory */
	stack = map_at(base + STACK_ADDRESS, STACK_SIZE, PROT_READ | PROT_WRITE);
#endif
	for (repeat = 0; repeat < repeats; repeat++)
	{
		uint64_t start_instructions, start_cycles, end_instructions, end_cycles, start, elapsed;

		counters(&start_instructions, &start_cycles);
		start = now();
		host_run_on_stack(run_call, NULL, stack + STACK_SIZE);
		elapsed = now() - start;
		counters(&end_instructions, &end_cycles);
		if (elapsed < best)
		{
			best = elapsed;
			best_instructions = end_instructions - start_instructions;
			best_cycles = end_cycles - start_cycles;
		}
	}
	printf("%u %llu %llu %llu\n", call.result, (unsigned long long)best, (unsigned long long)best_instructions,
		(unsigned long long)best_cycles);
	return 0;
}
