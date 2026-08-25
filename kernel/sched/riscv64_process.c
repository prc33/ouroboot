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
#include "mm/ramfs.h"

#define CSR_SSTATUS 0x100
#define SSTATUS_SPP  (1UL << 8)
#define SSTATUS_SPIE (1UL << 5)
#define SSTATUS_SIE  (1UL << 1)

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
	/* SPIE inherits the *current* global SIE, not a hardcoded 1: real
	 * sret semantics copy SPIE into SIE, and this kernel has no
	 * per-process interrupt-enable state of its own (arch/riscv64_timer.c's
	 * own comment) -- SIE is one global CPU-wide policy, so a freshly
	 * created process should come up under whatever that policy
	 * currently is, not silently override it back on. Real bug, found
	 * running real *paced* interactive input: hardcoding SPIE=1 here
	 * meant every new process's first launch re-enabled interrupts
	 * regardless of arch/riscv64_timer.c's timer_disable() (called once,
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

	p->root_table = new_root;
	p->pid = next_pid++;
	p->ppid = 0; /* no real parent -- created directly by kmain, not fork() */
	p->exit_code = 0;
	for (int fd = 0; fd < MAX_FDS; fd++)
		p->fds[fd].used = 0;

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
	 * checkpoint 6-10 test has finished (riscv64_kmain.c's
	 * run_process_test/run_fork_test/run_exec_test/run_init_test/
	 * run_interactive_test), so this is genuinely the last checkpoint
	 * in the chain right now. Will need the "P10" bumped (or replaced
	 * with a hook-supplied string) if a checkpoint 11 ever chains in
	 * after this one, same as every earlier "next checkpoint appends
	 * here" point in this kernel's own history. */
	kprintf("process: all processes exited\n");
	kprintf("P10 checkpoint OK\n");
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

int process_current_ppid(void) {
	return current_process ? current_process->ppid : 0;
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
 * ranges the same way), so fork() just clones the same fixed windows
 * every process is known to actually use -- 16MB from 0, comfortably
 * covering any of this kernel's real ELF payloads' code/data/BSS; the
 * 2-page stack at 0xB0000000 (process_create_from_elf's own
 * STACK_VA); and 1MB of arch/riscv64_syscall.c's own mmap arena
 * (MMAP_BASE, same value duplicated here rather than shared via a
 * header -- matches this function's existing "each fixed window
 * hardcoded where it's used" style).
 *
 * That third range is checkpoint 9's own real bug, found running
 * busybox ash for the first time: ash mmap()s a buffer for reading
 * its script *before* fork()ing to run an external command (this
 * function originally only cloned the first two ranges) -- the
 * forked child's own post-fork/pre-execve code (still running ash's
 * own binary, same as any real fork()) touched that buffer, which
 * its own COW-cloned address space never had mapped at all (not
 * missing a copy -- genuinely absent, unlike a stale-but-present COW
 * page), producing an unhandled load page fault. Every earlier
 * fork()-using test in this kernel used a program that never called
 * mmap, so this was invisible until a real, more complex program
 * exercised fork() and mmap() together. A process that mapped
 * anything outside these three windows still wouldn't survive a fork
 * -- true of every real program this checkpoint actually runs, but
 * not the general case, same spirit as mm/elf.c's own "no filesystem
 * yet" caveat elsewhere in this kernel. */
#define FORK_CLONE_LO 0x0UL
#define FORK_CLONE_HI 0x1000000UL   /* 16MB */
#define FORK_STACK_LO 0xB0000000UL
#define FORK_STACK_HI 0xB0002000UL  /* 2 pages, matches process_create_from_elf's own stack_pages */
#define FORK_MMAP_LO 0x60000000UL  /* arch/riscv64_syscall.c's MMAP_BASE */
#define FORK_MMAP_HI 0x60100000UL  /* 1MB -- comfortably past what any real fork()+mmap() test here actually uses */

int process_fork(struct regs *r) {
	struct process *parent = current_process;
	struct process *child = alloc_slot();
	if (!child)
		return -1;

	unsigned long *child_root = paging_new_addrspace();
	paging_fork_cow(child_root, parent->root_table, FORK_CLONE_LO, FORK_CLONE_HI);
	paging_fork_cow(child_root, parent->root_table, FORK_STACK_LO, FORK_STACK_HI);
	paging_fork_cow(child_root, parent->root_table, FORK_MMAP_LO, FORK_MMAP_HI);

	child->root_table = child_root;
	child->pid = next_pid++;
	child->ppid = parent->pid;
	child->exit_code = 0;

	/* Real fork() semantics: open file descriptors survive into the
	 * child. Simplified here to an independent copy (including `pos`)
	 * rather than a real shared-refcount file object -- real POSIX
	 * fork() has parent and child *share* the underlying offset (one
	 * advances, the other sees it too); nothing in this checkpoint's
	 * tests needs that, and this ramfs has no real inode table to hang
	 * a shared object off yet anyway. */
	for (int fd = 0; fd < MAX_FDS; fd++) {
		/* field-by-field, not a struct assignment -- TCC's codegen
		 * would ask for memmove() for that, which this freestanding
		 * kernel has never linked (same reason as process_trampoline's
		 * own copy_regs() above). */
		child->fds[fd].used = parent->fds[fd].used;
		child->fds[fd].data = parent->fds[fd].data;
		child->fds[fd].size = parent->fds[fd].size;
		child->fds[fd].pos = parent->fds[fd].pos;
	}

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
#define WNOHANG 1
#define ECHILD 10

long process_wait4(int pid, int *status_out, int options) {
	struct process *me = current_process;
	for (;;) {
		int have_matching_child = 0;
		for (int i = 0; i < MAX_PROCESSES; i++) {
			struct process *p = &processes[i];
			if (p->state == PROC_UNUSED || p->ppid != me->pid)
				continue;
			if (pid != -1 && p->pid != pid)
				continue;
			have_matching_child = 1;
			if (p->state == PROC_ZOMBIE) {
				int reaped_pid = p->pid;
				if (status_out)
					*status_out = (p->exit_code & 0xff) << 8; /* WIFEXITED/WEXITSTATUS-decodable, real Linux encoding */
				p->state = PROC_UNUSED; /* reaped -- slot free for reuse */
				return reaped_pid;
			}
		}
		if (!have_matching_child)
			return -ECHILD;
		if (options & WNOHANG)
			return 0;
		process_schedule();
	}
}

int process_fd_alloc(void) {
	for (int i = 0; i < MAX_FDS; i++)
		if (!current_process->fds[i].used) {
			current_process->fds[i].used = 1;
			return i;
		}
	return -1;
}

struct fd_entry *process_fd_get(int index) {
	if (index < 0 || index >= MAX_FDS || !current_process->fds[index].used)
		return 0;
	return &current_process->fds[index];
}

void process_fd_close(int index) {
	if (index >= 0 && index < MAX_FDS)
		current_process->fds[index].used = 0;
}

/* checkpoint 8: real execve() -- see process.h's own comment for the
 * contract. Same "new address space, elf_load, hand-built stack"
 * shape as process_create_from_elf(), the two real differences being
 * (a) this replaces the *current* process's root_table instead of
 * creating a new process, and (b) the stack carries real caller-
 * supplied argv instead of a single fixed arg0. */
#define EXECVE_MAX_ARGV 8
#define EXECVE_ARG_MAX 64

int process_execve(struct regs *r, const char *path, char **argv, char **envp) {
	(void)envp; /* real environment support is future scope -- every
	             * process in this kernel gets an empty one, same
	             * simplification process_create_from_elf already made */

	const struct ramfs_file *file = ramfs_lookup(path);
	if (!file)
		return -1;

	/* Snapshot argv strings into kernel memory *before* touching the
	 * address space they live in -- once the new address space is
	 * activated, `argv`/`argv[i]` (pointers into the *old* one) stop
	 * meaning anything. Bounded: nothing this kernel execve()s needs
	 * more than a handful of short arguments. */
	static char argbuf[EXECVE_MAX_ARGV][EXECVE_ARG_MAX];
	int argc = 0;
	while (argv && argv[argc] && argc < EXECVE_MAX_ARGV) {
		int i = 0;
		while (argv[argc][i] && i < EXECVE_ARG_MAX - 1) {
			argbuf[argc][i] = argv[argc][i];
			i++;
		}
		argbuf[argc][i] = 0;
		argc++;
	}

	struct process *p = current_process;
	unsigned long *prev_root = paging_active_root(); /* == p->root_table, still valid until we succeed */
	unsigned long *new_root = paging_new_addrspace();
	paging_activate(new_root);

	unsigned long entry = elf_load(file->data, file->size);
	if (!entry) {
		paging_activate(prev_root);
		return -1;
	}

	/* Stack -- same VA/size as process_create_from_elf, now with real
	 * argv instead of a single fixed arg0. String data goes in the
	 * top 256 bytes, the argc/argv/envp/auxv pointer block in the
	 * 256 bytes below that -- comfortably separate, both well clear
	 * of real stack use below. */
	unsigned long stack_va = 0xB0000000UL;
	unsigned long stack_pages = 2;
	for (unsigned long i = 0; i < stack_pages; i++) {
		unsigned long phys = pmm_alloc_page();
		paging_map_page(stack_va + i * PAGE_SIZE, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
	}
	unsigned long stack_top = stack_va + stack_pages * PAGE_SIZE;

	unsigned char *strp = (unsigned char *)(stack_top - 256);
	unsigned long argv_ptrs[EXECVE_MAX_ARGV];
	for (int a = 0; a < argc; a++) {
		int len = 0;
		while (argbuf[a][len])
			len++;
		for (int i = 0; i <= len; i++) /* <= to include the NUL */
			strp[i] = argbuf[a][i];
		argv_ptrs[a] = (unsigned long)strp;
		strp += len + 1;
	}

	unsigned long *sp = (unsigned long *)(stack_top - 512);
	int idx = 0;
	sp[idx++] = argc;
	for (int a = 0; a < argc; a++)
		sp[idx++] = argv_ptrs[a];
	sp[idx++] = 0; /* argv[] NULL terminator */
	sp[idx++] = 0; /* envp[0] = NULL */
	sp[idx++] = 6; /* auxv[0].a_type = AT_PAGESZ */
	sp[idx++] = PAGE_SIZE;
	sp[idx++] = 0; /* auxv[1] = AT_NULL */
	sp[idx++] = 0;

	/* Replace this process's address space in place -- old physical
	 * pages (code/data/stack of whatever was running before) are
	 * simply never freed. Documented leak, not an oversight: this
	 * kernel has no "tear down an address space" walk yet (same
	 * "no reclaim yet" simplification as arch/riscv64_syscall.c's
	 * sys_brk shrink path), and every process in this checkpoint's
	 * tests execve()s at most once. */
	p->root_table = new_root;

	/* Rewrite the live trapframe in place -- this *is* what makes the
	 * syscall "return" into the new program: every GPR real execve()
	 * doesn't promise to preserve gets zeroed (stale values from the
	 * old program have no business surviving into the new one), sp
	 * and sepc get the new program's real values, sstatus is left
	 * exactly as it already was (it's already correctly configured
	 * for "return to U-mode" -- we got here via a real ecall *from*
	 * U-mode, so SPP/SPIE are already right; recomputing it would
	 * just reproduce what's already there). */
	r->ra = 0; r->gp = 0; r->tp = 0;
	r->t0 = 0; r->t1 = 0; r->t2 = 0;
	r->s0 = 0; r->s1 = 0;
	r->a0 = 0; r->a1 = 0; r->a2 = 0; r->a3 = 0; r->a4 = 0; r->a5 = 0; r->a6 = 0; r->a7 = 0;
	r->s2 = 0; r->s3 = 0; r->s4 = 0; r->s5 = 0; r->s6 = 0; r->s7 = 0; r->s8 = 0; r->s9 = 0; r->s10 = 0; r->s11 = 0;
	r->t3 = 0; r->t4 = 0; r->t5 = 0; r->t6 = 0;
	r->sp = (unsigned long)sp;
	r->sepc = entry;

	return 0;
}
