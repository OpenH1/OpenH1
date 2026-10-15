/*
HOST_ENTER.S

Entering guest code when its memory lies at a base the host chooses
(HALO_GUEST_BASE; tools/guest_base_register.py): the guest's code finds the
base in x21, which it never writes, and uses x22 as its scratch register.
Both are callee-saved, so the host's own functions, which the guest calls,
keep them; the host's own values of them are saved here and put back.

Assembled for the ELF hosts (Linux, Android) and the Mach-O one (macOS).
*/

#ifdef __APPLE__
#define SYMBOL(name) _##name
#define FUNCTION(name) .globl SYMBOL(name) %% .p2align 2 %% SYMBOL(name):
#else
#define SYMBOL(name) name
#define FUNCTION(name) .globl name ; .type name, %function ; .p2align 2 ; name:
#endif

	.text

/* uint32_t host_enter_guest(uintptr_t function, uintptr_t base,
	uint32_t a, uint32_t b, uint32_t c, uint32_t d) */
FUNCTION(host_enter_guest)
	stp x29, x30, [sp, #-32]!
	mov x29, sp
	stp x21, x22, [sp, #16]
	mov x16, x0
	mov x21, x1
	/* (32-bit moves: the guest's arguments, zero-extended as arm64_32
	expects of its pointers) */
	mov w0, w2
	mov w1, w3
	mov w2, w4
	mov w3, w5
	blr x16
	ldp x21, x22, [sp, #16]
	ldp x29, x30, [sp], #32
	ret

/* void host_run_on_stack(void (*function)(void *), void *argument,
	void *stack_top): returns once function does, on the original stack */
FUNCTION(host_run_on_stack)
	stp x29, x30, [sp, #-32]!
	mov x29, sp
	str x19, [sp, #16]
	mov x19, sp
	and x2, x2, #0xfffffffffffffff0
	mov sp, x2
	mov x16, x0
	mov x0, x1
	blr x16
	mov sp, x19
	ldr x19, [sp, #16]
	ldp x29, x30, [sp], #32
	ret

#ifndef __APPLE__
	.section .note.GNU-stack, "", %progbits
#endif
