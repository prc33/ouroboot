/* Architecture-specific half of the process layer -- see
 * sched/process.c (the ~94%-generic half) and
 * docs/kernel-arch-split-plan.md for why this split exists and how
 * the two sides talk (sched/process.h's own "arch seam" section,
 * right after struct process). Everything here is genuinely riscv64:
 * struct regs's own layout, CSR/SSTATUS bits, the S-mode trap-return
 * mechanism (arch/risc/riscv64_trap_entry.S), and the hand-built initial
 * kernel-stack-frame convention arch/risc/riscv64_switch_context.S's
 * `addi sp,sp,-112` restore sequence expects.
 *
 * NOTE for a future arch/i386_process.c: i386's own arch/i386/paging.c has no
 * per-address-space API yet (paging_new_addrspace/paging_activate/
 * paging_fork_cow/paging_get_flags/paging_ensure_writable are all
 * riscv64-only today -- see mm/paging.h's own `#ifndef
 * KERNEL_ARCH_RISCV64` split), so sched/process.c can't actually link
 * for i386 until that's built too, independent of this file existing.
 * See docs/kernel-arch-split-plan.md for the concrete scope. */
#include "kernel.h"
#include "riscv64_trap.h"
#include "riscv64_memmap.h"
#include "mm/paging.h"
#include "sched/process.h"

#define CSR_SSTATUS 0x100
#define SSTATUS_SPP  (1UL << 8)
#define SSTATUS_SPIE (1UL << 5)
#define SSTATUS_SIE  (1UL << 1)

extern void riscv64_trap_return(void); /* arch/risc/riscv64_trap_entry.S */

/* sched/process.c's own copy_regs_bytes() (checkpoint 21) does the
 * actual copy now, shared with arch/i386/process.c's own equivalent
 * wrapper -- see process.h's comment on it. */
static void copy_regs(struct regs *dst, const struct regs *src) {
	copy_regs_bytes(dst, src, sizeof(struct regs));
}

/* Snapshots the live global trapframe into p->user_regs -- called on
 * a process's way *out* (sched/process.c's process_schedule(), right
 * before handing the CPU to someone else), since RV64_TRAPFRAME_BASE
 * is a single shared slot every process's own traps overwrite;
 * without this, whatever p was doing at the moment it yielded would
 * be lost the instant a second process trapped. */
void process_arch_save_trapframe(struct process *p) {
	copy_regs(&p->user_regs, (struct regs *)RV64_TRAPFRAME_BASE);
}

/* The other half of process_arch_save_trapframe(), and the one thing
 * every path that makes `p` "the process about to run in U-mode" must
 * do before that happens, whether p has never run before
 * (process_arch_trampoline) or is resuming after a previous
 * process_arch_save_trapframe() (process_schedule(), right after its
 * switch_context() call returns): reactivate p's own address space,
 * restore its own saved trapframe over whatever's currently in the
 * shared slot, and point the trap-stack indirection
 * (arch/risc/riscv64_trap_entry.S's own comment) at p's own kernel stack,
 * so if p traps again it lands on ITS stack, not whoever's was
 * current a moment ago. */
void process_arch_activate_and_restore(struct process *p) {
	paging_activate(p->root_table);
	copy_regs((struct regs *)RV64_TRAPFRAME_BASE, &p->user_regs);
	*(unsigned long *)RV64_CURRENT_KSTACK_PTR = (unsigned long)&p->kernel_stack[PROC_KSTACK_WORDS];
}

/* ra target for a process's hand-built initial kernel stack frame --
 * see process_arch_kstack_frame_init()'s own comment. Reads
 * process_get_current() rather than taking a parameter:
 * switch_context()'s restore sequence lands here via a bare `ret`,
 * the same shape as arch/risc/riscv64_task.c's task_a/task_b entry
 * functions, which take no arguments for the same reason.
 * sched/process.c's process_schedule()/process_run() always set the
 * current process before switching in, so it's correct here by
 * construction. */
void process_arch_trampoline(void) {
	process_arch_activate_and_restore(process_get_current());
	riscv64_trap_return();
	/* never reached: riscv64_trap_return ends in sret */
	for (;;) riscv_wfi();
}

/* Hand-built initial kernel-stack frame -- identical technique to
 * arch/risc/riscv64_task.c's task_init: 13 fake callee-saved registers
 * (ra pointing at process_arch_trampoline, s0-s11 unused/zero) plus 8
 * bytes padding for 16-byte alignment, matching
 * arch/risc/riscv64_switch_context.S's `addi sp,sp,-112` exactly. Shared
 * by sched/process.c's process_create_from_elf_argv() (a brand new
 * process) and process_fork() (a child resuming right where its
 * parent's fork() call returns, via the trapframe
 * process_arch_fork_child() already snapshotted). */
void process_arch_kstack_frame_init(struct process *p) {
	unsigned long *top = &p->kernel_stack[PROC_KSTACK_WORDS];
	unsigned long *frame = top - 14;
	frame[0] = (unsigned long)process_arch_trampoline;
	for (int j = 1; j < 13; j++)
		frame[j] = 0;
	p->kernel_sp = (unsigned long)frame;
}

/* Saved U-mode context a freshly created process starts at --
 * process_arch_trampoline seeds the real trapframe from this the
 * first time it runs. Every GPR except sp starts zeroed: a fresh
 * process's ABI contract only promises a valid sp and entry pc. */
void process_arch_init_context(struct process *p, unsigned long entry, unsigned long sp) {
	struct regs *ur = &p->user_regs;
	for (unsigned long *w = (unsigned long *)ur; w < (unsigned long *)(ur + 1); w++)
		*w = 0;
	ur->sepc = entry;
	ur->sp = sp;
	unsigned long sstatus = riscv_read_sstatus();
	sstatus &= ~SSTATUS_SPP;  /* sret drops to U-mode */
	/* SPIE inherits the *current* global SIE, not a hardcoded 1: real
	 * sret semantics copy SPIE into SIE, and this kernel has no
	 * per-process interrupt-enable state of its own (arch/risc/riscv64_timer.c's
	 * own comment) -- SIE is one global CPU-wide policy, so a freshly
	 * created process should come up under whatever that policy
	 * currently is, not silently override it back on. Real bug, found
	 * running real *paced* interactive input: hardcoding SPIE=1 here
	 * meant every new process's first launch re-enabled interrupts
	 * regardless of arch/risc/riscv64_timer.c's timer_disable() (called once,
	 * right after the P4 scheduler checkpoint) -- the timer came back
	 * the moment checkpoint 5 created its first process, and stayed
	 * back for every process after, defeating timer_disable() entirely
	 * and leaving every syscall's busy-wait loop (sys_read's above all
	 * -- the one paced real-world delays actually exercise) exposed to
	 * the exact nested-trap corruption timer_disable() exists to
	 * prevent (see its own comment for the full mechanism). */
	if (sstatus & SSTATUS_SIE)
		sstatus |= SSTATUS_SPIE;
	else
		sstatus &= ~SSTATUS_SPIE;
	ur->sstatus = sstatus;
}

/* Child's saved trapframe: an exact snapshot of the parent's live
 * regs at this ecall (same registers, same sepc -- both processes
 * resume right after the same `ecall` instruction), except a0,
 * fork()'s return value, forced to 0 -- "you are the child" is
 * the *only* thing that needs to differ between the two copies
 * for this to be a correct fork(). The parent's own a0 (this
 * child's pid) is set by arch/risc/riscv64_syscall.c's sys_clone
 * itself, on its live `r`, same as any other syscall's return
 * value -- untouched by this copy. */
void process_arch_fork_child(struct process *child, struct regs *parent_regs) {
	copy_regs(&child->user_regs, parent_regs);
	child->user_regs.a0 = 0;
}

/* Rewrites the live trapframe in place -- this *is* what makes the
 * execve() syscall "return" into the new program: every GPR real
 * execve() doesn't promise to preserve gets zeroed (stale values from
 * the old program have no business surviving into the new one), sp
 * and sepc get the new program's real values, sstatus is left exactly
 * as it already was (it's already correctly configured for "return to
 * U-mode" -- we got here via a real ecall *from* U-mode, so SPP/SPIE
 * are already right; recomputing it would just reproduce what's
 * already there). */
void process_arch_execve_rewrite(struct regs *r, unsigned long entry, unsigned long sp) {
	r->ra = 0; r->gp = 0; r->tp = 0;
	r->t0 = 0; r->t1 = 0; r->t2 = 0;
	r->s0 = 0; r->s1 = 0;
	r->a0 = 0; r->a1 = 0; r->a2 = 0; r->a3 = 0; r->a4 = 0; r->a5 = 0; r->a6 = 0; r->a7 = 0;
	r->s2 = 0; r->s3 = 0; r->s4 = 0; r->s5 = 0; r->s6 = 0; r->s7 = 0; r->s8 = 0; r->s9 = 0; r->s10 = 0; r->s11 = 0;
	r->t3 = 0; r->t4 = 0; r->t5 = 0; r->t6 = 0;
	r->sp = sp;
	r->sepc = entry;
}
