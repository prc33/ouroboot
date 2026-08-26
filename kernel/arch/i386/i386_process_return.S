/* void i386_trap_return(struct regs *r) -- never returns to its
 * caller; execution continues in ring3 wherever `r` says.
 *
 * Exact copy of isr_stubs.S's isr_common_stub epilogue (pop segment
 * regs, popa, skip int_no/err_code, iret), except it starts from an
 * arbitrary caller-supplied pointer instead of "whatever the CPU just
 * pushed on entry" -- arch/i386/process.c's process_arch_trampoline()
 * (a brand new process, launched via switch_context()'s bare `ret`,
 * never took a real trap at all) and a fork()ed child's very first
 * resume both need to enter ring3 from a `struct regs` that's just
 * sitting in a process's own user_regs field, not on any stack. struct
 * regs's own field order (idt.h) is laid out to match exactly what
 * this sequence expects to pop, so pointing esp straight at it and
 * running the same sequence works unchanged -- same trick this
 * kernel's isr_stubs.S already relies on implicitly (via `iret` alone,
 * for the CPU-preserved eip/cs/eflags/useresp/ss) every ordinary
 * syscall return already takes. */
.globl i386_trap_return
i386_trap_return:
	movl 4(%esp), %esp
	pop %eax
	movw %ax, %ds
	pop %eax
	movw %ax, %es
	pop %eax
	movw %ax, %fs
	pop %eax
	movw %ax, %gs
	popa
	add $8, %esp
	sti
	iret
