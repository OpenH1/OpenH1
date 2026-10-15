/*
BENCH.LD

Layout of the benchmark's guest image (tools/base_register_bench/run.py), as
port/android/guest/guest.ld lays out the game's: at the guest address
0x88000000, data on its own pages.
*/

ENTRY(bench_run)

SECTIONS
{
	. = 0x88000000;
	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	. = ALIGN(0x10000);
	.data : { *(.data .data.*) }
	.bss : { *(.bss .bss.*) *(COMMON) }
	/DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) *(.guest_init) *(.guest_fini) }
}
