/* General process table -- checkpoint 6. Real, independent U-mode
 * processes: each gets its own address space (mm/paging.h's
 * paging_new_addrspace/paging_activate), its own kernel stack, and a
 * saved trapframe restored via arch/riscv64_trap_entry.S's
 * riscv64_trap_return label -- see that file's own comment for why
 * unifying "trap stack" and "process kernel stack" is what makes a
 * process able to genuinely block mid-syscall (this checkpoint's
 * SYS_sched_yield) and be resumed later exactly where it left off.
 *
 * Two distinct "kernel-side execution" mechanisms coexist here, both
 * ultimately switch_context() (sched/riscv64_switch_context.S),
 * unchanged from P4's task scheduler -- it's a generic "swap callee-
 * saved regs + sp, ret" coroutine primitive that's never cared what
 * call chain it's swapping:
 *   1. A process being dispatched *for the first time*: its
 *      kernel_sp is a hand-built initial frame (same technique as
 *      sched/riscv64_task.c's task_init) whose `ra` is
 *      process_trampoline -- switch_context's `ret` jumps straight
 *      there, which activates the process's address space, seeds the
 *      global trapframe from its saved user_regs, and falls into
 *      riscv64_trap_return to actually enter U-mode.
 *   2. A process *resuming after a blocking syscall*: its kernel_sp
 *      is wherever process_schedule()'s own switch_context() call
 *      left off, deep inside that process's own C call stack (e.g.
 *      inside sys_sched_yield). Resuming here just continues that C
 *      code normally; it eventually returns out through
 *      syscall_dispatch/trap_dispatch and falls into trap_entry.S's
 *      restore-and-sret tail via the ordinary `jalr t0` call site,
 *      the same path every non-blocking syscall already used in
 *      P1-P5.
 * Both end up executing in U-mode via the exact same restore code
 * (arch/riscv64_trap_entry.S), just entered two different ways.
 */
#include "kernel.h"
#include "arch/riscv64_trap.h"
#include "arch/riscv64_memmap.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/elf.h"
#include "sched/task.h" /* switch_context() -- shared with the P4 task scheduler */
#include "sched/process.h"

#define CSR_SSTATUS 0x100
#define SSTATUS_SPP  (1UL << 8)
#define SSTATUS_SPIE (1UL << 5)

extern void riscv64_trap_return(void); /* arch/riscv64_trap_entry.S */

static struct process processes[MAX_PROCESSES];
static struct process *current_process;
static int next_pid = 1;
static int process_mode = 0;

static struct process *alloc_slot(void) {
	for (int i = 0; i < MAX_PROCESSES; i++)
		if (processes[i].state == PROC_UNUSED)
			return &processes[i];
	return 0;
}

void process_init(void) {
	for (int i = 0; i < MAX_PROCESSES; i++)
		processes[i].state = PROC_UNUSED;
	current_process = 0;
}

/* struct assignment would ask TCC's codegen for memmove(), which this
 * freestanding kernel has never linked (every other byte copy in it,
 * e.g. mm/riscv64_paging.c's COW handler, is a plain word/byte loop
 * for the same reason) -- copy by hand instead. */
static void copy_regs(struct regs *dst, const struct regs *src) {
	const unsigned long *s = (const unsigned long *)src;
	unsigned long *d = (unsigned long *)dst;
	for (unsigned int i = 0; i < sizeof(struct regs) / sizeof(unsigned long); i++)
		d[i] = s[i];
}

/* Snapshots the live global trapframe into p->user_regs -- called on
 * a process's way *out* (process_schedule(), right before handing the
 * CPU to someone else), since RV64_TRAPFRAME_BASE is a single shared
 * slot every process's own traps overwrite; without this, whatever
 * p was doing at the moment it yielded would be lost the instant a
 * second process trapped. */
static void save_trapframe(struct process *p) {
	copy_regs(&p->user_regs, (struct regs *)RV64_TRAPFRAME_BASE);
}

/* The other half of save_trapframe(), and the one thing every path
 * that makes `p` "the process about to run in U-mode" must do before
 * that happens, whether p has never run before (process_trampoline)
 * or is resuming after a previous save_trapframe() (process_schedule(),
 * right after its switch_context() call returns): reactivate p's own
 * address space, restore its own saved trapframe over whatever's
 * currently in the shared slot, and point the trap-stack indirection
 * (arch/riscv64_trap_entry.S's own comment) at p's own kernel stack,
 * so if p traps again it lands on ITS stack, not whoever's was
 * current a moment ago. Getting this step (or save_trapframe) wrong
 * showed up, the first time this file was written, as both test
 * processes' output printing the *second* process's label rather than
 * their own -- state which never actually belonged to A "resuming" at
 * all, since nothing had reasserted A's own saved context before its
 * call chain fell back through the shared trap-return path with B's
 * data still sitting in the shared slot. */
static void activate_and_restore(struct process *p) {
	paging_activate(p->root_table);
	copy_regs((struct regs *)RV64_TRAPFRAME_BASE, &p->user_regs);
	*(unsigned long *)RV64_CURRENT_KSTACK_PTR = (unsigned long)&p->kernel_stack[PROC_KSTACK_WORDS];
}

/* ra target for a process's hand-built initial kernel stack frame --
 * see the file comment's mechanism (1). Reads current_process rather
 * than taking a parameter: switch_context()'s restore sequence lands
 * here via a bare `ret`, the same shape as sched/riscv64_task.c's
 * task_a/task_b entry functions, which take no arguments for the same
 * reason. process_schedule()/process_run() always set current_process
 * before switching in, so it's correct here by construction. */
static void process_trampoline(void) {
	activate_and_restore(current_process);
	riscv64_trap_return();
	/* never reached: riscv64_trap_return ends in sret */
	for (;;) __builtin_riscv_wfi();
}

struct process *process_create_from_elf(const unsigned char *elf_data, unsigned long elf_size, const char *arg0) {
	struct process *p = alloc_slot();
	if (!p)
		return 0;

	unsigned long *prev_root = paging_active_root();
	unsigned long *new_root = paging_new_addrspace();
	paging_activate(new_root);

	unsigned long entry = elf_load(elf_data, elf_size);
	if (!entry) {
		paging_activate(prev_root);
		return 0;
	}

	/* User stack -- same layout/rationale as riscv64_kmain.c's
	 * run_elf_test (argc=1, argv={arg0,NULL}, envp empty, one real
	 * auxv entry: AT_PAGESZ, which musl's __libc_start_main has no
	 * fallback for -- see that function's comment for why it's not
	 * optional). Each process gets its own copy at the same virtual
	 * address, 0xB0000000 -- safe because it's a *different* address
	 * space now, unlike P5's single shared one. */
	unsigned long stack_va = 0xB0000000UL;
	unsigned long stack_pages = 2;
	for (unsigned long i = 0; i < stack_pages; i++) {
		unsigned long phys = pmm_alloc_page();
		paging_map_page(stack_va + i * PAGE_SIZE, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
	}
	unsigned long stack_top = stack_va + stack_pages * PAGE_SIZE;

	unsigned char *page = (unsigned char *)stack_top;
	char *argv0 = (char *)(page - 64);
	int i = 0;
	for (; arg0[i]; i++)
		argv0[i] = arg0[i];
	argv0[i] = 0;

	unsigned long *sp = (unsigned long *)(page - 128);
	sp[0] = 1;                    /* argc */
	sp[1] = (unsigned long)argv0; /* argv[0] */
	sp[2] = 0;                    /* argv[1] = NULL */
	sp[3] = 0;                    /* envp[0] = NULL */
	sp[4] = 6;                    /* auxv[0].a_type = AT_PAGESZ */
	sp[5] = PAGE_SIZE;             /* auxv[0].a_val */
	sp[6] = 0;                    /* auxv[1].a_type = AT_NULL */
	sp[7] = 0;

	/* Saved U-mode context this process starts at -- process_trampoline
	 * seeds the real trapframe from this the first time it runs.
	 * Every GPR except sp starts zeroed: a fresh process's ABI
	 * contract only promises a valid sp and entry pc, matching
	 * run_elf_test's own enter_usermode(entry, sp) call. */
	struct regs *ur = &p->user_regs;
	for (unsigned long *w = (unsigned long *)ur; w < (unsigned long *)(ur + 1); w++)
		*w = 0;
	ur->sepc = entry;
	ur->sp = (unsigned long)sp;
	unsigned long sstatus = __builtin_riscv_csrr(CSR_SSTATUS);
	sstatus &= ~SSTATUS_SPP;  /* sret drops to U-mode */
	sstatus |= SSTATUS_SPIE;  /* interrupts enabled once there */
	ur->sstatus = sstatus;

	p->root_table = new_root;
	p->pid = next_pid++;
	p->ppid = 0; /* no real parent -- created directly by kmain, not fork() */
	p->exit_code = 0;

	/* Hand-built initial kernel-stack frame -- identical technique to
	 * sched/riscv64_task.c's task_init: 13 fake callee-saved registers
	 * (ra pointing at process_trampoline, s0-s11 unused/zero) plus 8
	 * bytes padding for 16-byte alignment, matching
	 * sched/riscv64_switch_context.S's `addi sp,sp,-112` exactly. */
	unsigned long *top = &p->kernel_stack[PROC_KSTACK_WORDS];
	unsigned long *frame = top - 14;
	frame[0] = (unsigned long)process_trampoline;
	for (int j = 1; j < 13; j++)
		frame[j] = 0;
	p->kernel_sp = (unsigned long)frame;

	p->state = PROC_RUNNABLE;

	paging_activate(prev_root);
	return p;
}

/* Cooperative round-robin, starting the search just after whichever
 * process is current -- same shape as sched/riscv64_task.c's
 * task_yield, generalized from a fixed 2 slots to MAX_PROCESSES. A
 * no-op (returns immediately, doesn't even touch switch_context) if
 * this process is the only RUNNABLE one, which matters here in a way
 * it didn't for the fixed 2-task version: with just one live process
 * left, "yield to the next runnable process" has to mean "keep
 * running", not "switch to yourself" (switch_context()'s save/restore
 * of its *own* stack into itself is harmless, but pointless). */
void process_schedule(void) {
	struct process *old = current_process;
	int start = (int)(old - processes);
	int idx = start;
	struct process *next = 0;
	for (int i = 0; i < MAX_PROCESSES; i++) {
		idx = (idx + 1) % MAX_PROCESSES;
		if (processes[idx].state == PROC_RUNNABLE) {
			next = &processes[idx];
			break;
		}
	}
	if (!next || next == old)
		return;
	save_trapframe(old);
	current_process = next;
	switch_context(&old->kernel_sp, next->kernel_sp);
	/* Execution resumes here later, whenever `old` (== us: this whole
	 * function's stack frame, locals included, is exactly what
	 * switch_context() suspended and is now resuming) is switched
	 * back to. Whoever ran in between has left satp/the shared
	 * trapframe/the trap-stack pointer aimed at THEM -- reassert our
	 * own before falling back through to trap_entry.S's restore-and-
	 * sret tail, the same as process_trampoline() does for a process
	 * running for the first time (see activate_and_restore's own
	 * comment). */
	activate_and_restore(old);
}

/* What to do once the process table completely drains (no RUNNABLE
 * process left) -- riscv64_kmain.c sets this to chain into the next
 * checkpoint's own test (e.g. checkpoint 6's sched_yield test handing
 * off to checkpoint 7's fork test) the same way arch/riscv64_syscall.c's
 * sys_exit_group/sys_exit already chain P4->P5 checkpoint 1->P5
 * checkpoint 2. A hook is expected to create new processes and call
 * process_run() again (or not return at all, if it halts on its own);
 * it's cleared before being called so a *second* drain (this new
 * batch of processes finishing) falls through to the real halt below
 * rather than re-invoking the same hook. */
static void (*drain_hook)(void) = 0;

void process_set_drain_hook(void (*hook)(void)) {
	drain_hook = hook;
}

static void halt_process_test(void) __attribute__((noreturn));
static void halt_process_test(void) {
	/* Reached only once the process table has drained *and* no drain
	 * hook chained in another test -- currently that means every
	 * checkpoint 6/7 test has finished (riscv64_kmain.c's
	 * run_process_test/run_fork_test), so this is genuinely the last
	 * checkpoint in the chain right now. Will need the "P7" bumped
	 * (or replaced with a hook-supplied string) if a checkpoint 8 ever
	 * chains in after this one, same as every earlier "next checkpoint
	 * appends here" point in this kernel's own history. */
	kprintf("process: all processes exited\n");
	kprintf("P7 checkpoint OK\n");
	kprintf("halting.\n");
	for (;;) __builtin_riscv_wfi();
}

void process_exit_current(int exit_code) {
	current_process->state = PROC_ZOMBIE;
	current_process->exit_code = exit_code;

	struct process *next = 0;
	for (int i = 0; i < MAX_PROCESSES; i++)
		if (processes[i].state == PROC_RUNNABLE) {
			next = &processes[i];
			break;
		}

	if (!next) {
		if (drain_hook) {
			void (*hook)(void) = drain_hook;
			drain_hook = 0;
			hook();
		}
		halt_process_test();
	}

	/* `old` (the zombie we're leaving) is never resumed again, so its
	 * kernel_sp is dead from here on -- still passed to switch_context
	 * (its signature needs a valid store address) but nothing will
	 * ever read it back out. */
	struct process *old = current_process;
	current_process = next;
	switch_context(&old->kernel_sp, next->kernel_sp);
	for (;;) __builtin_riscv_wfi(); /* unreachable */
}

void process_run(struct process *first) {
	process_mode = 1;
	static unsigned long discard_sp;
	current_process = first;
	switch_context(&discard_sp, first->kernel_sp);
	/* not expected to return: every process's exit eventually reaches
	 * halt_process_test() above */
}

int process_mode_active(void) {
	return process_mode;
}

int process_current_pid(void) {
	return current_process ? current_process->pid : 0;
}

/* checkpoint 7: real fork(), via SYS_clone -- see process.h's own
 * comment for why clone() rather than a dedicated fork syscall, and
 * arch/riscv64_syscall.c for the narrow flags check that gates
 * getting here at all.
 *
 * The [lo,hi) ranges cloned below are a real, deliberate
 * simplification, not the general case: this checkpoint has no
 * per-process VMA list recording what a process has actually mapped
 * (process_create_from_elf hardcodes its own ELF-load and stack
 * ranges the same way), so fork() just clones the same two fixed
 * windows every process is known to actually use -- 16MB from 0,
 * comfortably covering any of this kernel's real ELF payloads' code/
 * data/BSS, plus the 2-page stack at 0xB0000000
 * (process_create_from_elf's own STACK_VA). A process that mapped
 * anything outside those (a real brk()/mmap() user, or a stack that
 * grew past 2 pages) wouldn't have it survive a fork -- true of this
 * checkpoint's own test but not the general case, same spirit as
 * mm/elf.c's own "no filesystem yet" caveat elsewhere in this
 * kernel. */
#define FORK_CLONE_LO 0x0UL
#define FORK_CLONE_HI 0x1000000UL   /* 16MB */
#define FORK_STACK_LO 0xB0000000UL
#define FORK_STACK_HI 0xB0002000UL  /* 2 pages, matches process_create_from_elf's own stack_pages */

int process_fork(struct regs *r) {
	struct process *parent = current_process;
	struct process *child = alloc_slot();
	if (!child)
		return -1;

	unsigned long *child_root = paging_new_addrspace();
	paging_fork_cow(child_root, parent->root_table, FORK_CLONE_LO, FORK_CLONE_HI);
	paging_fork_cow(child_root, parent->root_table, FORK_STACK_LO, FORK_STACK_HI);

	child->root_table = child_root;
	child->pid = next_pid++;
	child->ppid = parent->pid;
	child->exit_code = 0;

	/* Child's saved trapframe: an exact snapshot of the parent's live
	 * regs at this ecall (same registers, same sepc -- both processes
	 * resume right after the same `ecall` instruction), except a0,
	 * fork()'s return value, forced to 0 -- "you are the child" is
	 * the *only* thing that needs to differ between the two copies
	 * for this to be a correct fork(). The parent's own a0 (this
	 * child's pid) is set by arch/riscv64_syscall.c's sys_clone
	 * itself, on its live `r`, same as any other syscall's return
	 * value -- untouched by this copy. */
	copy_regs(&child->user_regs, r);
	child->user_regs.a0 = 0;

	/* Hand-built initial kernel-stack frame -- identical technique to
	 * process_create_from_elf: when this child is first scheduled, it
	 * resumes via process_trampoline using the trapframe snapshot
	 * just taken above, i.e. picks up exactly at the return-from-
	 * fork() point, in its own address space, with a0=0. */
	unsigned long *top = &child->kernel_stack[PROC_KSTACK_WORDS];
	unsigned long *frame = top - 14;
	frame[0] = (unsigned long)process_trampoline;
	for (int j = 1; j < 13; j++)
		frame[j] = 0;
	child->kernel_sp = (unsigned long)frame;

	child->state = PROC_RUNNABLE;
	return child->pid;
}

/* checkpoint 7: real wait4(), restricted to what's actually exercised
 * (see process.h's comment). Blocking here means the same thing
 * SYS_sched_yield's process_schedule() already does: spin, cooperatively
 * yielding to every other RUNNABLE process each time round, until the
 * condition holds. A real kernel would move the caller to a distinct
 * BLOCKED state and only reconsider it once the specific child it's
 * waiting on actually exits (avoiding the wasted table scans), but
 * "spin-yield until true" is the same real simplification this
 * kernel's P4 task scheduler already uses for its own timer-driven
 * wait (`while (g_ticks - last_tick < TICKS_PER_SWITCH) wfi();`) --
 * consistent with the rest of this codebase, not a new shortcut. */
long process_wait4(int pid, int *status_out) {
	struct process *me = current_process;
	for (;;) {
		for (int i = 0; i < MAX_PROCESSES; i++) {
			struct process *p = &processes[i];
			if (p->state == PROC_ZOMBIE && p->ppid == me->pid &&
			    (pid == -1 || p->pid == pid)) {
				int reaped_pid = p->pid;
				if (status_out)
					*status_out = (p->exit_code & 0xff) << 8; /* WIFEXITED/WEXITSTATUS-decodable, real Linux encoding */
				p->state = PROC_UNUSED; /* reaped -- slot free for reuse */
				return reaped_pid;
			}
		}
		process_schedule();
	}
}
