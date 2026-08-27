/* Architecture-specific half of the process layer -- see
 * sched/process.c (the ~94%-generic half) and
 * docs/kernel-arch-split-plan.md for why this split exists and how the
 * two sides talk (sched/process.h's own "arch seam" section). Mirrors
 * arch/risc/riscv64_process.c's own seven functions; genuinely
 * different here because i386's own trap mechanics are: struct regs's
 * own layout (idt.h), CR3/no-satp-equivalent register access, and --
 * the one real structural difference from riscv64, not just a renamed
 * equivalent -- there's no single fixed trapframe address. riscv64's
 * RV64_TRAPFRAME_BASE is one shared slot every trap overwrites, so
 * every process's context genuinely has to be saved/restored around
 * every switch. i386's trapframe instead lives on whichever kernel
 * stack the trap actually occurred on -- each process's own
 * kernel_stack (struct process) -- so a process resuming mid-syscall
 * (process_schedule()'s ordinary switch_context() return) just
 * continues its own frozen C call stack straight back to its own
 * still-intact trapframe, no restore needed at all. Only two cases
 * ever need i386_process_return.S's explicit "build ring3 entry from a
 * struct regs sitting in memory, not on a stack" path: a brand new
 * process's first-ever launch (process_arch_trampoline) and a fork()ed
 * child's first resume (its user_regs is a snapshot, not a live call
 * stack either). process_arch_save_trapframe()/process_arch_activate_and_restore()
 * still exist and are still called unconditionally around every switch
 * (same contract as riscv64's), but for i386 the "restore" half is
 * really just "make sure this process's own address space and TSS
 * kernel-stack pointer are live again" -- the trapframe itself needs
 * no help getting back to where it needs to be. */
#include "kernel.h"
#include "idt.h"
#include "mm/paging.h"
#include "sched/process.h"

extern void i386_trap_return(struct regs *r); /* arch/i386/i386_process_return.S */
extern void tss_set_kernel_stack(unsigned int esp0); /* arch/i386/gdt.c */

/* sched/process.c's own copy_regs_bytes() (checkpoint 21) does the
 * actual copy now, shared with arch/risc/riscv64_process.c's own
 * equivalent wrapper -- see process.h's comment on it. */
static void copy_regs(struct regs *dst, const struct regs *src) {
	copy_regs_bytes(dst, src, sizeof(struct regs));
}

/* Snapshots the live trapframe into p->user_regs -- called on a
 * process's way *out* (sched/process.c's process_schedule(), right
 * before handing the CPU to someone else). For i386 this is strictly
 * defensive, not load-bearing the way it is for riscv64 (see this
 * file's own header comment): idt_current_trapframe() at this point
 * already points at p's own still-live trapframe, sitting on p's own
 * kernel_stack, which nothing overwrites while p isn't running -- but
 * taking the snapshot anyway keeps this function's contract identical
 * across both arches and costs nothing. */
void process_arch_save_trapframe(struct process *p) {
	struct regs *live = idt_current_trapframe();
	if (live)
		copy_regs(&p->user_regs, live);
}

/* Reactivates p's own address space and points the TSS at p's own
 * kernel stack, so a future trap from p lands where it should -- both
 * needed whether p is resuming its own frozen call stack (process_schedule())
 * or is about to be entered fresh via i386_trap_return (process_arch_trampoline(),
 * below). Doesn't touch the trapframe itself -- see this file's own
 * header comment for why i386 doesn't need to.
 *
 * Re-installs p's own TLS descriptor too -- real bug, found running
 * real self-hosted TCC for the first time: unlike everything else
 * here, GDT slot 6 (arch/i386/gdt.c's gdt_set_tls_entry(), what
 * SYS_set_thread_area installs into) is a single global, CPU-wide
 * resource, not per-process state a trapframe save/restore touches at
 * all. Without this, whichever process last called set_thread_area()
 * (i.e. whichever real binary started up most recently) "won" the
 * slot for everyone -- any other process resuming afterward read its
 * own TLS pointer (%gs:0, e.g. musl's own pthread_self()/errno)
 * through a descriptor pointing at a *different* process's TLS block
 * entirely, faulting the instant it dereferenced anything through it.
 * See struct process's own tls_base comment for the full story. */
void process_arch_activate_and_restore(struct process *p) {
	paging_activate(p->root_table);
	tss_set_kernel_stack((unsigned int)(unsigned long)&p->kernel_stack[PROC_KSTACK_WORDS]);
	gdt_set_tls_entry(6, (unsigned int)p->tls_base);
}

/* ra target for a process's hand-built initial kernel stack frame --
 * see process_arch_kstack_frame_init()'s own comment. Reads
 * process_get_current() rather than taking a parameter: switch_context()'s
 * restore sequence lands here via a bare `ret`, same shape as
 * arch/risc/riscv64_process.c's own process_arch_trampoline(). */
void process_arch_trampoline(void) {
	struct process *p = process_get_current();
	process_arch_activate_and_restore(p);
	i386_trap_return(&p->user_regs);
	/* never reached: i386_trap_return ends in iret */
	for (;;) __asm__ volatile ("cli\n hlt");
}

/* Hand-built initial kernel-stack frame matching arch/i386/switch_context.S's
 * own save/restore order exactly: it pushes ebx,esi,edi,ebp (in that
 * order) and restores by popping ebp,edi,esi,ebx then `ret`s, so a
 * hand-built frame needs those four (any value -- callee-saved regs a
 * process has never actually set yet) below a return address of
 * process_arch_trampoline. Same technique as arch/i386/task.c's
 * task_init and arch/risc/riscv64_process.c's own
 * process_arch_kstack_frame_init(). Shared by
 * sched/process.c's process_create_from_elf_argv() (a brand new
 * process) and process_fork() (a child resuming right where its
 * parent's fork() call returns, via the trapframe
 * process_arch_fork_child() already snapshotted). */
void process_arch_kstack_frame_init(struct process *p) {
	unsigned long *top = &p->kernel_stack[PROC_KSTACK_WORDS];
	unsigned long *frame = top - 5; /* ebp, edi, esi, ebx, return-address */
	frame[0] = 0; /* ebp */
	frame[1] = 0; /* edi */
	frame[2] = 0; /* esi */
	frame[3] = 0; /* ebx */
	frame[4] = (unsigned long)process_arch_trampoline;
	p->kernel_sp = (unsigned long)frame;
}

/* Saved U-mode context a freshly created process starts at --
 * i386_trap_return seeds ring3 from this the first time it runs, via
 * process_arch_trampoline. Matches arch/i386/usermode.S's own
 * enter_usermode() setup exactly (same selectors, same IF-set eflags):
 * this *is* what enter_usermode built by hand for the very first P5
 * ring3 demo, now built into a saveable struct instead of pushed
 * straight onto the stack. */
void process_arch_init_context(struct process *p, unsigned long entry, unsigned long sp) {
	struct regs *ur = &p->user_regs;
	unsigned int *w = (unsigned int *)ur;
	for (unsigned int i = 0; i < sizeof(struct regs) / sizeof(unsigned int); i++)
		w[i] = 0;
	ur->ds = ur->es = ur->fs = ur->gs = 0x23; /* user data selector: GDT index 4, RPL 3 */
	ur->eip = (unsigned int)entry;
	ur->cs = 0x1B; /* user code selector: GDT index 3, RPL 3 */
	ur->eflags = 0x200; /* IF set -- interrupts stay enabled in user mode, same as enter_usermode.S. No riscv64-style "inherit the current global interrupt-enable policy" subtlety here: i386 interrupts are unconditionally enabled well before process_init() ever runs (idt_init()/pic_init()), so there's no "timer disabled for checkpoint scaffolding" state to accidentally re-enable (arch/risc/riscv64_process.c's own comment on SSTATUS_SPIE is riscv64-only for this reason). */
	ur->useresp = (unsigned int)sp;
	ur->ss = 0x23;
}

/* Child's saved trapframe: an exact snapshot of the parent's live regs
 * at this int $0x80 (same registers, same eip -- both processes resume
 * right after the same instruction), except eax, clone()'s return
 * value, forced to 0 -- "you are the child" is the only thing that
 * needs to differ. The parent's own eax (this child's pid, or -1) is
 * set by arch/i386/syscall.c's own sys_clone, on its live `r`, same as
 * any other syscall's return value -- untouched by this copy. */
void process_arch_fork_child(struct process *child, struct regs *parent_regs) {
	copy_regs(&child->user_regs, parent_regs);
	child->user_regs.eax = 0;
}

/* Rewrites the live trapframe in place -- this *is* what makes the
 * execve() syscall "return" into the new program: every GPR real
 * execve() doesn't promise to preserve gets zeroed, eip/useresp get
 * the new program's real values, everything else (cs/ss/eflags/segment
 * selectors) is left exactly as it already was -- already correctly
 * configured for "return to ring3", since we got here via a real
 * int $0x80 *from* ring3. */
void process_arch_execve_rewrite(struct regs *r, unsigned long entry, unsigned long sp) {
	r->edi = 0; r->esi = 0; r->ebp = 0; r->ebx = 0; r->edx = 0; r->ecx = 0; r->eax = 0;
	r->eip = (unsigned int)entry;
	r->useresp = (unsigned int)sp;
}
