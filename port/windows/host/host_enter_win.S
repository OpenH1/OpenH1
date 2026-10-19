/*
HOST_ENTER_WIN.S

Moving a thread onto a stack in guest memory (host_win.h,
host_run_on_stack): the x32 guest keeps stack addresses in 32-bit
registers, so every thread that runs guest code needs its stack below 4 GB,
which Windows does not give a thread (host_thread_win.c). The stack pointer
moves there, and the thread's stack limits in its TEB move with it, as
SwitchToFiber moves them: Windows checks the stack pointer against them
(the stack probes of __chkstk, the exception dispatcher's walk, the
stack's growth), and so does the C runtime.

	NT_TIB.StackBase           %gs:0x08    the stack's top
	NT_TIB.StackLimit          %gs:0x10    its committed bottom
	TEB.DeallocationStack      %gs:0x1478  its reservation's bottom

The new stack's first frame is host_stack_bottom's, whose exception
handler (host_stack_exception, host_memory_watch_win.c) reports a crash
that no handler above took: the exception dispatcher walks no further than
the stack its limits name, so the thread's own handlers, on its first
stack, are never reached.

Each function has unwinding information (the .seh directives), so that
Windows's unwinder, and a debugger, walk from the new stack back to the
thread's own.

Assembled for x86_64-pc-windows-msvc; the Microsoft x64 calling convention.
*/

	.text

/* void host_run_on_stack(void (*function)(void *), void *argument,
	void *stack_top, void *stack_limit, void *deallocation) */
	.globl host_run_on_stack
	.def host_run_on_stack
	.scl 2
	.type 32
	.endef
	.p2align 4
host_run_on_stack:
	.seh_proc host_run_on_stack
	pushq %rbp
	.seh_pushreg %rbp
	pushq %rbx
	.seh_pushreg %rbx
	pushq %rsi
	.seh_pushreg %rsi
	pushq %rdi
	.seh_pushreg %rdi
	subq $40, %rsp
	.seh_stackalloc 40
	movq %rsp, %rbp
	.seh_setframe %rbp, 0
	.seh_endprologue
	/* (the fifth argument, above the four registers' home space, the
	return address, the four registers saved and the 40 bytes) */
	movq 112(%rbp), %rax
	/* the thread's own limits, kept in callee-saved registers */
	movq %gs:0x08, %rbx
	movq %gs:0x10, %rsi
	movq %gs:0x1478, %rdi
	movq %r8, %gs:0x08
	movq %r9, %gs:0x10
	movq %rax, %gs:0x1478
	movq %r8, %rsp
	andq $-16, %rsp
	/* (the callee's home space) */
	subq $32, %rsp
	/* host_stack_bottom(function, argument): already in rcx and rdx */
	callq host_stack_bottom
	movq %rbp, %rsp
	movq %rbx, %gs:0x08
	movq %rsi, %gs:0x10
	movq %rdi, %gs:0x1478
	addq $40, %rsp
	popq %rdi
	popq %rsi
	popq %rbx
	popq %rbp
	retq
	.seh_endproc

/* void host_stack_bottom(void (*function)(void *), void *argument): the new
stack's first frame, whose handler reports a crash */
	.def host_stack_bottom
	.scl 3
	.type 32
	.endef
	.p2align 4
host_stack_bottom:
	.seh_proc host_stack_bottom
	.seh_handler host_stack_exception, @except
	subq $40, %rsp
	.seh_stackalloc 40
	.seh_endprologue
	movq %rcx, %rax
	movq %rdx, %rcx
	callq *%rax
	/* (the return address stays inside this function, not its epilogue) */
	nop
	addq $40, %rsp
	retq
	.seh_endproc
