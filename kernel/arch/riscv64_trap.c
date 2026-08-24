/* RISC-V trap dispatch. Unlike i386's 256-entry IDT, there's exactly
 * ONE hardware vector (`stvec`) for every trap -- exceptions,
 * interrupts, and `ecall` (our syscall gate) alike -- so dispatch
 * happens entirely in software here, keyed on `scause`.
 *
 * arch/riscv64_trap_entry.S is the raw machine code `stvec` points
 * at; it saves all GPRs + the four trap CSRs into the fixed global
 * struct regs at RV64_TRAPFRAME_BASE, switches to a dedicated trap
 * stack, and calls trap_dispatch() below via a function pointer this
 * file writes into RV64_TRAP_DISPATCH_PTR at init time -- see
 * arch/riscv64_memmap.h for why raw asm can't call trap_dispatch()
 * directly (no relocation support for hand-written .S files). */
#include "kernel.h"
#include "riscv64_trap.h"
#include "riscv64_memmap.h"

#define CSR_STVEC    0x105
#define CSR_SSTATUS  0x100
#define SSTATUS_SUM  (1UL << 18)
#define SCAUSE_INTERRUPT_BIT (1UL << 63)

extern void riscv64_trap_entry(void); /* arch/riscv64_trap_entry.S */

/* Exceptions: scause low bits when the interrupt bit is clear.
 * Interrupts: scause low bits when the interrupt bit is set (only
 * cause 5, supervisor timer, is used in this kernel -- no PLIC/
 * external-interrupt support, matching the existing scope: the i386
 * side never does interrupt-driven I/O either, only the PIT timer). */
#define NUM_CAUSES 16
static void (*exception_handlers[NUM_CAUSES])(struct regs *);
static void (*interrupt_handlers[NUM_CAUSES])(struct regs *);
static void (*syscall_handler)(struct regs *);

static const char *exception_names[NUM_CAUSES] = {
	"Instruction address misaligned", "Instruction access fault",
	"Illegal instruction", "Breakpoint",
	"Load address misaligned", "Load access fault",
	"Store/AMO address misaligned", "Store/AMO access fault",
	"Environment call from U-mode", "Environment call from S-mode",
	"Reserved", "Reserved",
	"Instruction page fault", "Load page fault",
	"Reserved", "Store/AMO page fault",
};

void trap_dispatch(struct regs *r) {
	unsigned long cause = r->scause;

	if (cause & SCAUSE_INTERRUPT_BIT) {
		unsigned long n = cause & ~SCAUSE_INTERRUPT_BIT;
		if (n < NUM_CAUSES && interrupt_handlers[n]) {
			interrupt_handlers[n](r);
			return;
		}
		kprintf("\n!! UNHANDLED INTERRUPT %lu\n", n);
		kprintf("FATAL: unhandled interrupt, halting\n");
		for (;;) __builtin_riscv_wfi();
	}

	if (cause == 8) { /* ecall from U-mode -- our one syscall gate */
		r->sepc += 4; /* ecall doesn't auto-advance sepc like x86's int; skip past it, or we'd re-execute it forever */
		if (syscall_handler)
			syscall_handler(r);
		else
			kprintf("FATAL: ecall with no syscall handler registered\n");
		return;
	}

	if (cause < NUM_CAUSES && exception_handlers[cause]) {
		exception_handlers[cause](r);
		return;
	}

	kprintf("\n!! UNHANDLED EXCEPTION %lu: %s (stval=%p) sepc=%p\n",
		cause, cause < NUM_CAUSES ? exception_names[cause] : "Reserved",
		(void *)r->stval, (void *)r->sepc);
	kprintf("FATAL: unhandled exception, halting\n");
	for (;;) __builtin_riscv_wfi();
}

void isr_register_handler(int cause, void (*handler)(struct regs *)) {
	if (cause >= 0 && cause < NUM_CAUSES)
		exception_handlers[cause] = handler;
}

void irq_register_handler(int cause, void (*handler)(struct regs *)) {
	if (cause >= 0 && cause < NUM_CAUSES)
		interrupt_handlers[cause] = handler;
}

void syscall_set_handler(void (*handler)(struct regs *)) {
	syscall_handler = handler;
}

void trap_init(void) {
	*(void (**)(struct regs *))RV64_TRAP_DISPATCH_PTR = trap_dispatch;
	/* Default: the same single dedicated trap stack every checkpoint
	 * before sched/riscv64_process.c used, until that subsystem starts
	 * (right before it dispatches its first process) and repoints this
	 * at whichever process is about to run -- see
	 * arch/riscv64_trap_entry.S's own comment for why this indirection
	 * exists at all. Every trap before that point (the COW/ring3/ELF-
	 * loader checkpoints) behaves exactly as it always did: this never
	 * changes, so it's still effectively one fixed trap stack for
	 * them. */
	*(unsigned long *)RV64_CURRENT_KSTACK_PTR = RV64_TRAP_STACK_TOP;
	__builtin_riscv_csrw(CSR_STVEC, (unsigned long)riscv64_trap_entry);

	/* sstatus.SUM ("permit Supervisor User Memory access"): without
	 * it, S-mode is architecturally forbidden from touching any page
	 * whose PTE has the U bit set -- a real security feature (stops
	 * the kernel from accidentally trusting a raw user pointer), but
	 * one this kernel's syscall handlers need disabled, since e.g.
	 * arch/riscv64_syscall.c's sys_write_impl reads straight out of a
	 * user-supplied buffer through the very same page table (there's
	 * only one -- no separate per-process address spaces yet). Found
	 * by booting the ring3 test: the first syscall (write) page-
	 * faulted *in the kernel's own sys_write_impl*, not in the U-mode
	 * payload, reading the U-only-accessible string it was asked to
	 * print. i386 has no equivalent bit; PTE_USER there simply always
	 * permits ring0 access too, no separate opt-in. */
	unsigned long sstatus = __builtin_riscv_csrr(CSR_SSTATUS);
	__builtin_riscv_csrw(CSR_SSTATUS, sstatus | SSTATUS_SUM);
	kprintf("trap: stvec installed (direct mode, one vector for exceptions+interrupts+ecall)\n");
}
