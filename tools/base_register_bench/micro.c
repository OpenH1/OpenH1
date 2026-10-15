/*
MICRO.C

The cost of the base register's instruction patterns on this core
(tools/base_register_bench/run.py --micro), natively: a chain of dependent
loads (each address the previous load's result, as in pointer chasing)
and a stream of independent ones, each

- plain: ldr x0, [x0, #8] (the address in the register, as without the base
  register);
- register offset: ldr w0, [x21, w0, uxtw] (the rewrite of an access at
  offset 0: no extra instruction);
- 32-bit add: add w22, w0, #8 ; ldr w0, [x21, w22, uxtw] (the rewrite of an
  access at an offset, as LFI does it);
- extending add: add x22, x21, w0, uxtw ; ldr w0, [x22, #8] (the rewrite of
  an access at an offset by the instructions without the register-offset
  form: ldp, exclusives, SIMD structures).

The chain lies in one 4 GB-aligned span, x21 being its base, so the 32-bit
offsets name the same memory as the full addresses. Prints nanoseconds and
cycles per load.
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

#define NODES 1024
#define STEPS 200000000ULL

static uint64_t now(void)
{
	struct timespec value;

	clock_gettime(CLOCK_MONOTONIC, &value);
	return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
}

static uint64_t cycles(void)
{
#ifdef __APPLE__
	struct rusage_info_v4 information;

	if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&information) == 0)
		return information.ri_cycles;
#endif
	return 0;
}

/* each node: the next node's full address at 8, and its 32-bit offset at 0
(offset 0, the register-offset form) and 8 (the added form reads the low
half of the full address) */
struct node
{
	uint32_t next_offset;
	uint32_t padding;
	uint64_t next;
	uint64_t pad[6];
};

static void report(const char *name, uint64_t nanoseconds, uint64_t cycle_count)
{
	printf("%-28s %6.3f ns", name, (double)nanoseconds / STEPS);
	if (cycle_count)
		printf("  %5.2f cycles", (double)cycle_count / STEPS);
	printf("\n");
}

int main(void)
{
	/* a 4 GB-aligned span, so that a node's low 32 bits are its offset */
	char *reservation = mmap(NULL, 0x200000000ULL, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	uintptr_t base = ((uintptr_t)reservation + 0xffffffffULL) & ~(uintptr_t)0xffffffffULL;
	struct node *nodes = mmap((void *)(base + 0x10000000), NODES * sizeof(struct node), PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
	uint32_t order[NODES];
	uint64_t start, start_cycles, steps;
	uint64_t pointer;
	int index;

	if (reservation == MAP_FAILED || nodes == MAP_FAILED)
	{
		perror("mmap");
		return 1;
	}
	srand(7);
	for (index = 0; index < NODES; index++)
		order[index] = (uint32_t)index;
	for (index = NODES - 1; index > 0; index--)
	{
		int other = rand() % (index + 1);
		uint32_t swap = order[index];

		order[index] = order[other];
		order[other] = swap;
	}
	for (index = 0; index < NODES; index++)
	{
		struct node *next = &nodes[order[(index + 1) % NODES]];

		nodes[order[index]].next = (uint64_t)(uintptr_t)next;
		nodes[order[index]].next_offset = (uint32_t)(uintptr_t)next;
	}

	/* dependent loads */
	pointer = (uint64_t)(uintptr_t)&nodes[order[0]];
	start_cycles = cycles();
	start = now();
	for (steps = STEPS; steps; steps -= 4)
		__asm__ volatile("ldr %0, [%0, #8]\n\tldr %0, [%0, #8]\n\tldr %0, [%0, #8]\n\tldr %0, [%0, #8]"
			: "+r"(pointer));
	report("dependent, plain", now() - start, cycles() - start_cycles);

	pointer = (uint32_t)(uintptr_t)&nodes[order[0]];
	start_cycles = cycles();
	start = now();
	for (steps = STEPS; steps; steps -= 4)
		__asm__ volatile("mov x21, %1\n\t"
			"ldr %w0, [x21, %w0, uxtw]\n\tldr %w0, [x21, %w0, uxtw]\n\t"
			"ldr %w0, [x21, %w0, uxtw]\n\tldr %w0, [x21, %w0, uxtw]"
			: "+r"(pointer) : "r"(base) : "x21");
	report("dependent, register offset", now() - start, cycles() - start_cycles);

	pointer = (uint32_t)(uintptr_t)&nodes[order[0]];
	start_cycles = cycles();
	start = now();
	for (steps = STEPS; steps; steps -= 4)
		__asm__ volatile("mov x21, %1\n\t"
			"add x22, x21, %w0, uxtw\n\tldr %w0, [x22, #8]\n\tadd x22, x21, %w0, uxtw\n\tldr %w0, [x22, #8]\n\t"
			"add x22, x21, %w0, uxtw\n\tldr %w0, [x22, #8]\n\tadd x22, x21, %w0, uxtw\n\tldr %w0, [x22, #8]"
			: "+r"(pointer) : "r"(base) : "x21", "x22");
	report("dependent, extending add", now() - start, cycles() - start_cycles);

	pointer = (uint32_t)(uintptr_t)&nodes[order[0]];
	start_cycles = cycles();
	start = now();
	for (steps = STEPS; steps; steps -= 4)
		__asm__ volatile("mov x21, %1\n\t"
			"add w22, %w0, #8\n\tldr %w0, [x21, w22, uxtw]\n\tadd w22, %w0, #8\n\tldr %w0, [x21, w22, uxtw]\n\t"
			"add w22, %w0, #8\n\tldr %w0, [x21, w22, uxtw]\n\tadd w22, %w0, #8\n\tldr %w0, [x21, w22, uxtw]"
			: "+r"(pointer) : "r"(base) : "x21", "x22");
	report("dependent, 32-bit add", now() - start, cycles() - start_cycles);

	/* independent loads (from one node, 4 at a time) */
	{
		uint64_t a, b, c, d;
		uint64_t full = (uint64_t)(uintptr_t)&nodes[0];
		uint32_t offset = (uint32_t)(uintptr_t)&nodes[0];

		start_cycles = cycles();
		start = now();
		for (steps = STEPS; steps; steps -= 4)
			__asm__ volatile("ldr %0, [%4, #8]\n\tldr %1, [%4, #16]\n\tldr %2, [%4, #24]\n\tldr %3, [%4, #32]"
				: "=&r"(a), "=&r"(b), "=&r"(c), "=&r"(d) : "r"(full));
		report("independent, plain", now() - start, cycles() - start_cycles);

		start_cycles = cycles();
		start = now();
		for (steps = STEPS; steps; steps -= 4)
			__asm__ volatile("mov x21, %5\n\t"
				"add x22, x21, %w4, uxtw\n\tldr %0, [x22, #8]\n\tadd x22, x21, %w4, uxtw\n\tldr %1, [x22, #16]\n\t"
				"add x22, x21, %w4, uxtw\n\tldr %2, [x22, #24]\n\tadd x22, x21, %w4, uxtw\n\tldr %3, [x22, #32]"
				: "=&r"(a), "=&r"(b), "=&r"(c), "=&r"(d) : "r"(offset), "r"(base) : "x21", "x22");
		report("independent, extending add", now() - start, cycles() - start_cycles);
	}
	return 0;
}
