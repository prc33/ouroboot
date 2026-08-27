/* General process table -- checkpoint 6. Real, independent U-mode
 * processes: each gets its own address space (mm/paging.h's
 * paging_new_addrspace/paging_activate), its own kernel stack, and a
 * saved trapframe restored via the architecture's own trap-return
 * mechanism (process_arch_activate_and_restore() -- see this file's
 * own "arch seam" comment in process.h) -- see arch/risc/riscv64_trap_entry.S's
 * own comment for why unifying "trap stack" and "process kernel
 * stack" is what makes a process able to genuinely block mid-syscall
 * (this checkpoint's SYS_sched_yield) and be resumed later exactly
 * where it left off.
 *
 * checkpoint 16 (docs/kernel-arch-split-plan.md): this file used to be
 * sched/riscv64_process.c, and was about 94% architecture-neutral
 * already -- see docs/kernel-complexity-review.md section 12's own
 * measurement. The genuinely riscv64-specific ~6% (struct regs's own
 * layout, CSR/SSTATUS bits, the trap-return mechanism, the hand-built
 * initial-kernel-stack-frame convention switch_context() expects) now
 * lives in arch/risc/riscv64_process.c instead, behind the small
 * process_arch_*() interface declared in process.h. This file only
 * ever touches `struct regs` opaquely (as a pointer to snapshot/pass
 * along), never a named field of it.
 *
 * Two distinct "kernel-side execution" mechanisms coexist here, both
 * ultimately switch_context() (arch/risc/riscv64_switch_context.S),
 * unchanged from P4's task scheduler -- it's a generic "swap callee-
 * saved regs + sp, ret" coroutine primitive that's never cared what
 * call chain it's swapping:
 *   1. A process being dispatched *for the first time*: its
 *      kernel_sp is a hand-built initial frame (same technique as
 *      arch/risc/riscv64_task.c's task_init) whose `ra` is
 *      process_arch_trampoline -- switch_context's `ret` jumps
 *      straight there, which activates the process's address space,
 *      seeds the global trapframe from its saved user_regs, and falls
 *      into the architecture's own trap-return path to actually enter
 *      U-mode.
 *   2. A process *resuming after a blocking syscall*: its kernel_sp
 *      is wherever process_schedule()'s own switch_context() call
 *      left off, deep inside that process's own C call stack (e.g.
 *      inside sys_sched_yield). Resuming here just continues that C
 *      code normally; it eventually returns out through
 *      syscall_dispatch/trap_dispatch and falls into the trap entry
 *      code's restore-and-return tail via the ordinary call site, the
 *      same path every non-blocking syscall already used in P1-P5.
 * Both end up executing in U-mode via the exact same restore code,
 * just entered two different ways.
 */
#include "kernel.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/elf.h"
#include "sched/task.h" /* switch_context() -- shared with the P4 task scheduler */
#include "sched/process.h"
#include "mm/ramfs.h"

#define USER_BRK_BASE 0x40000000UL
#define USER_MMAP_BASE 0x60000000UL
#define USER_STACK_TOP 0xB1000000UL
#define USER_STACK_LIMIT 0xB0000000UL
#define USER_STACK_INITIAL_PAGES 2

static struct process processes[MAX_PROCESSES];
static struct process *current_process;
static int next_pid = 1;
static int process_mode = 0;

/* For arch/risc/riscv64_process.c's own process_arch_trampoline() -- see
 * process.h's own comment on why that reads this rather than taking a
 * parameter. */
struct process *process_get_current(void) {
	return current_process;
}

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

/* checkpoint 15: the single canonical user-stack/argv/envp/auxv
 * builder -- process_create_from_elf() and process_execve() used to
 * each hand-roll their own nearly-identical version (see
 * docs/kernel-complexity-review.md section 3). This is process_execve()'s
 * own version, kept as the one canonical implementation rather than
 * process_create_from_elf()'s older one: it's the one that's actually
 * been stress-tested end to end -- every self-hosted TCC compile
 * (kernel/test/selfhost.sh) runs its own deeply recursive parser
 * through exactly this stack, including its lazy growth
 * (process_handle_stack_fault(), below) down to USER_STACK_LIMIT.
 *
 * kernel/test/riscv64_checkpoints.c's own run_elf_test() deliberately
 * stays unconverged -- see that file's own comment for why (it runs
 * before there's a process table at all, so there's no `struct
 * process` for this to write into).
 *
 * Maps and zeroes enough pages for the actual argument data (with two
 * pages as the minimum), records the committed range, writes the
 * NUL-terminated strings followed by the
 * argc/argv/envp/auxv pointer block (envp always empty, one real
 * auxv entry -- AT_PAGESZ, which musl's __libc_start_main has no
 * fallback for) into PTRBLOCK_SIZE bytes below that. Returns the
 * resulting sp. Every `argv[i]` must already be kernel-resident by the
 * time this is called -- process_execve()'s own caller is responsible
 * for snapshotting real (user-space) argv strings first, since this
 * function runs *after* the new address space is already active. */
#define EXECVE_ARG_MAX 256

static unsigned long build_user_stack(struct process *p, char *const argv[], int argc) {
	unsigned long string_bytes = 0;
	for (int a = 0; a < argc; a++) {
		unsigned long len = 1;
		while (argv[a][len - 1]) len++;
		string_bytes += len;
	}
	unsigned long pointer_bytes = (unsigned long)(argc + 7) * sizeof(unsigned long);
	unsigned long needed = string_bytes + pointer_bytes + 15;
	unsigned long stack_pages = (needed + PAGE_SIZE - 1) / PAGE_SIZE;
	if (stack_pages < USER_STACK_INITIAL_PAGES)
		stack_pages = USER_STACK_INITIAL_PAGES;
	unsigned long stack_top = USER_STACK_TOP;
	unsigned long stack_va = stack_top - stack_pages * PAGE_SIZE;
	for (unsigned long i = 0; i < stack_pages; i++) {
		unsigned long phys = pmm_alloc_page();
		/* pmm_alloc_page() does not clear memory; user stacks must not
		 * expose contents left by an earlier allocation. */
		unsigned long *words = (unsigned long *)phys;
		for (unsigned int w = 0; w < PAGE_SIZE / sizeof(unsigned long); w++)
			words[w] = 0;
		paging_map_page(stack_va + i * PAGE_SIZE, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
	}
	p->user_stack_lo = stack_va;
	p->user_stack_hi = stack_top;
	p->user_stack_limit = USER_STACK_LIMIT;
	p->user_brk = USER_BRK_BASE;
	p->user_mmap_next = USER_MMAP_BASE;

	unsigned char *strp = (unsigned char *)(stack_top - string_bytes);
	unsigned long argv_ptrs[PROCESS_EXEC_MAX_ARGS];
	for (int a = 0; a < argc; a++) {
		int len = 0;
		while (argv[a][len])
			len++;
		for (int i = 0; i <= len; i++) /* <= to include the NUL */
			strp[i] = argv[a][i];
		argv_ptrs[a] = (unsigned long)strp;
		strp += len + 1;
	}

	unsigned long *sp = (unsigned long *)((unsigned long)(stack_top - string_bytes - pointer_bytes) & ~15UL);
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
	return (unsigned long)sp;
}

/* Real argv[] variant -- riscv64_kmain.c's product boot uses this
 * directly (argv={"ash","-i",0}), execve()ing straight into BusyBox
 * ash rather than through a separate tiny wrapper ELF whose only job
 * was supplying that second argument (process_create_from_elf()'s
 * single-arg0 signature below couldn't carry it). `argv` must already
 * be kernel-resident (see build_user_stack()'s own comment) -- true of
 * every caller today, all of which pass plain string literals. */
struct process *process_create_from_elf_argv(const unsigned char *elf_data, unsigned long elf_size, char *const argv[], int argc) {
	struct process *p = alloc_slot();
	if (!p)
		return 0;

	unsigned long *prev_root = paging_active_root();
	unsigned long *new_root = paging_new_addrspace();
	paging_activate(new_root);

	unsigned long entry = elf_load(elf_data, elf_size);
	if (!entry) {
		paging_activate(prev_root);
		paging_destroy_addrspace(new_root);
		return 0;
	}

	p->cwd[0] = '/';
	p->cwd[1] = 0;

	unsigned long sp = build_user_stack(p, argv, argc);
	process_arch_init_context(p, entry, sp);

	p->root_table = new_root;
	p->pid = next_pid++;
	p->ppid = 0; /* no real parent -- created directly by kmain, not fork() */
	p->exit_code = 0;
	for (int fd = 0; fd < MAX_FDS; fd++)
		p->fds[fd].used = 0;

	process_arch_kstack_frame_init(p);

	p->state = PROC_RUNNABLE;

	paging_activate(prev_root);
	return p;
}

/* Single-arg0 convenience wrapper -- every current checkpoint-chain
 * caller (kernel/test/riscv64_checkpoints.c's process_from_initrd())
 * only ever needs argv={label,NULL} (proc_test_riscv64.c's own
 * comment: it reads argv[0][0] as its printed label, "A"/"B"). Thin on
 * purpose: the real work is process_create_from_elf_argv() above. */
struct process *process_create_from_elf(const unsigned char *elf_data, unsigned long elf_size, const char *arg0) {
	char *argv[] = { (char *)arg0, 0 };
	return process_create_from_elf_argv(elf_data, elf_size, argv, 1);
}

/* Cooperative round-robin, starting the search just after whichever
 * process is current -- same shape as arch/risc/riscv64_task.c's
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
	process_arch_save_trapframe(old);
	current_process = next;
	switch_context(&old->kernel_sp, next->kernel_sp);
	/* Execution resumes here later, whenever `old` (== us: this whole
	 * function's stack frame, locals included, is exactly what
	 * switch_context() suspended and is now resuming) is switched
	 * back to. Whoever ran in between has left the active address
	 * space/the shared trapframe/the trap-stack pointer aimed at
	 * THEM -- reassert our own before falling back through to the
	 * trap-return path, the same as process_arch_trampoline() does
	 * for a process running for the first time (see
	 * process_arch_activate_and_restore()'s own comment). */
	process_arch_activate_and_restore(old);
}

/* What to do once the process table completely drains (no RUNNABLE
 * process left) -- kernel/test/riscv64_checkpoints.c sets this to
 * chain into the next checkpoint's own test (e.g. checkpoint 6's
 * sched_yield test handing off to checkpoint 7's fork test) the same
 * way arch/risc/riscv64_syscall.c's own pre-process-exit hook chains
 * P4->P5 checkpoint 1->P5 checkpoint 2, before there's a process table
 * to drain at all. A hook is expected to create new processes and call
 * process_run() again (or not return at all, if it halts on its own);
 * it's cleared before being called so a *second* drain (this new
 * batch of processes finishing) falls through to the real halt below
 * rather than re-invoking the same hook. The product boot never
 * registers one at all, so it always falls straight through to
 * process_halt() the first time its own top-level process exits. */
static void (*drain_hook)(void) = 0;

void process_set_drain_hook(void (*hook)(void)) {
	drain_hook = hook;
}

/* Reached whenever the process table drains (no RUNNABLE process left)
 * with no drain hook registered to chain into anything further -- the
 * ordinary end of any real boot, product included, once its one
 * top-level process (or, for kernel/test/riscv64_checkpoints.c's own
 * chain, its last checkpoint) exits. Deliberately knows nothing about
 * checkpoints: kernel/test/riscv64_checkpoints.c's own
 * finish_checkpoint_boot() prints its closing "P10 checkpoint OK"
 * itself, via process_set_drain_hook(), before calling this -- see
 * that file and process.h's own comment on process_halt(). */
void process_halt(void) {
	kprintf("process: all processes exited\n");
	kprintf("halting.\n");
	arch_halt_forever();
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
		process_halt();
	}

	/* `old` (the zombie we're leaving) is never resumed again, so its
	 * kernel_sp is dead from here on -- still passed to switch_context
	 * (its signature needs a valid store address) but nothing will
	 * ever read it back out. */
	struct process *old = current_process;
	current_process = next;
	switch_context(&old->kernel_sp, next->kernel_sp);
	arch_halt_forever(); /* unreachable */
}

void process_run(struct process *first) {
	process_mode = 1;
	static unsigned long discard_sp;
	current_process = first;
	switch_context(&discard_sp, first->kernel_sp);
	/* not expected to return: every process's exit eventually reaches
	 * process_halt() above */
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

unsigned long process_current_brk(void) {
	return current_process ? current_process->user_brk : USER_BRK_BASE;
}

void process_set_current_brk(unsigned long value) {
	if (current_process)
		current_process->user_brk = value;
}

unsigned long process_take_mmap(unsigned long length) {
	unsigned long base = current_process->user_mmap_next;
	current_process->user_mmap_next += length;
	return base;
}

void process_note_mmap_end(unsigned long end) {
	if (current_process && end > current_process->user_mmap_next)
		current_process->user_mmap_next = end;
}

int process_handle_stack_fault(unsigned long address) {
	if (!current_process || address < current_process->user_stack_limit ||
	    address >= current_process->user_stack_hi)
		return 0;
	unsigned long page = address & ~(PAGE_SIZE - 1UL);
	unsigned long phys = pmm_alloc_page();
	if (!phys)
		return 0;
	unsigned long *words = (unsigned long *)phys;
	for (unsigned int i = 0; i < PAGE_SIZE / sizeof(unsigned long); i++)
		words[i] = 0;
	paging_map_page(page, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
	if (page < current_process->user_stack_lo)
		current_process->user_stack_lo = page;
	return 1;
}

const char *process_current_cwd(void) {
	return current_process ? current_process->cwd : "/";
}

void process_set_current_cwd(const char *path) {
	if (!current_process) return;
	int i = 0;
	while (path[i] && i < 127) { current_process->cwd[i] = path[i]; i++; }
	current_process->cwd[i] = 0;
}

/* checkpoint 7: real fork(), via SYS_clone -- see process.h's own
 * comment for why clone() rather than a dedicated fork syscall, and
 * arch/risc/riscv64_syscall.c for the narrow flags check that gates
 * getting here at all.
 *
 * The [lo,hi) ranges cloned below are a real, deliberate
 * simplification, not the general case: this checkpoint has no
 * per-process VMA list recording what a process has actually mapped
 * (process_create_from_elf hardcodes its own ELF-load and stack
 * ranges the same way), so fork() just clones the same fixed windows
 * every process is known to actually use -- 16MB from 0, comfortably
 * covering any of this kernel's real ELF payloads' code/data/BSS; the
 * process's actual mapped stack at 0xB0000000; and 1MB of
 * arch/risc/riscv64_syscall.c's own mmap arena
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
int process_fork(struct regs *r) {
	struct process *parent = current_process;
	struct process *child = alloc_slot();
	if (!child)
		return -1;

	unsigned long *child_root = paging_new_addrspace();
	paging_fork_user(child_root, parent->root_table);

	child->root_table = child_root;
	child->pid = next_pid++;
	child->ppid = parent->pid;
	child->exit_code = 0;
	child->user_stack_lo = parent->user_stack_lo;
	child->user_stack_hi = parent->user_stack_hi;
	child->user_stack_limit = parent->user_stack_limit;
	child->user_brk = parent->user_brk;
	child->user_mmap_next = parent->user_mmap_next;
	for (int i = 0; i < 128; i++) child->cwd[i] = parent->cwd[i];

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
		 * kernel has never linked (same reason as
		 * arch/risc/riscv64_process.c's own copy_regs()). */
		child->fds[fd].used = parent->fds[fd].used;
		child->fds[fd].data = parent->fds[fd].data;
		child->fds[fd].size = parent->fds[fd].size;
		child->fds[fd].pos = parent->fds[fd].pos;
		child->fds[fd].is_dir = parent->fds[fd].is_dir;
		child->fds[fd].dynfile = parent->fds[fd].dynfile;
		for (int i = 0; i < 128; i++) child->fds[fd].path[i] = parent->fds[fd].path[i];
	}
	/* checkpoint 12: any active stdio redirection (dup3() onto fd
	 * 0/1/2 -- struct process's own stdio_override comment) survives
	 * fork() too, same as fds[] above and for the same reason (real
	 * fd table semantics: fork()'d children share their parent's open
	 * files, redirected stdio included -- a subshell running inside
	 * an already-redirected `> file` block needs to keep writing to
	 * that file, not suddenly see the console again). */
	for (int fd = 0; fd < 3; fd++) {
		child->stdio_override[fd].used = parent->stdio_override[fd].used;
		child->stdio_override[fd].data = parent->stdio_override[fd].data;
		child->stdio_override[fd].size = parent->stdio_override[fd].size;
		child->stdio_override[fd].pos = parent->stdio_override[fd].pos;
		child->stdio_override[fd].is_dir = parent->stdio_override[fd].is_dir;
		child->stdio_override[fd].dynfile = parent->stdio_override[fd].dynfile;
		for (int i = 0; i < 128; i++) child->stdio_override[fd].path[i] = parent->stdio_override[fd].path[i];
	}

	/* Child resumes exactly where the parent's fork() call returns,
	 * with a0=0 ("you are the child"); see
	 * arch/risc/riscv64_process.c's process_arch_fork_child() for how the
	 * trapframe snapshot is taken. */
	process_arch_fork_child(child, r);
	process_arch_kstack_frame_init(child);

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
				paging_destroy_addrspace(p->root_table);
				p->root_table = 0;
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

/* checkpoint 12: writes into a *specific* fd slot by index -- unlike
 * process_fd_alloc() (which picks the first free slot), dup3()'s
 * target fd is a specific number the caller chose, closing whatever
 * was there first (real dup2/dup3 semantics). */
void process_fd_set(int index, const struct fd_entry *src) {
	if (index < 0 || index >= MAX_FDS)
		return;
	struct fd_entry *dst = &current_process->fds[index];
	dst->data = src->data;
	dst->size = src->size;
	dst->pos = src->pos;
	dst->is_dir = src->is_dir;
	dst->dynfile = src->dynfile;
	for (int i = 0; i < 128; i++) dst->path[i] = src->path[i];
	dst->used = 1;
}

/* checkpoint 12: real dup2()/dup3() onto fd 0/1/2 -- see process.h's
 * own comment on stdio_override for why these exist at all instead of
 * just using fds[]. `fd` must be 0/1/2; out-of-range is a caller bug
 * (arch/risc/riscv64_syscall.c's sys_dup3/sys_read/sys_write/sys_close are
 * the only callers, and they all check first), not something these
 * bother reporting. */
/* current_process is NULL until a real process has actually been
 * scheduled (process_init() itself sets it to 0; it's only ever
 * non-NULL from process_run()/process_schedule() onward) -- but
 * fd 0/1/2 writes/reads happen from the very first checkpoint 5 ring3
 * test onward, which runs via enter_usermode() directly and never
 * touches the process subsystem at all. Real bug, found running this
 * exact test after adding these functions: the earliest sys_write()
 * call (checkpoint 5's own "hello from ring3 via ecall") crashed with
 * a NULL-pointer page fault the instant it reached here. A NULL
 * current_process legitimately means "no real process context, so
 * definitely not redirected" -- these three all treat it that way,
 * consistently. */
struct fd_entry *process_stdio_get(int fd) {
	if (!current_process || fd < 0 || fd > 2 || !current_process->stdio_override[fd].used)
		return 0;
	return &current_process->stdio_override[fd];
}

void process_stdio_set(int fd, const struct fd_entry *src) {
	if (!current_process || fd < 0 || fd > 2)
		return;
	/* field-by-field, not a struct assignment -- see process_fork's
	 * own comment on why. */
	struct fd_entry *dst = &current_process->stdio_override[fd];
	dst->data = src->data;
	dst->size = src->size;
	dst->pos = src->pos;
	dst->is_dir = src->is_dir;
	dst->dynfile = src->dynfile;
	for (int i = 0; i < 128; i++) dst->path[i] = src->path[i];
	dst->used = 1;
}

void process_stdio_clear(int fd) {
	if (current_process && fd >= 0 && fd <= 2)
		current_process->stdio_override[fd].used = 0;
}

/* checkpoint 8: real execve() -- see process.h's own comment for the
 * contract. Same "new address space, elf_load, build_user_stack()"
 * shape as process_create_from_elf_argv() above, the one real
 * difference being that this replaces the *current* process's
 * root_table instead of creating a new process. */
int process_execve(struct regs *r, const char *path, char **argv, char **envp) {
	(void)envp; /* real environment support is future scope -- every
	             * process in this kernel gets an empty one, same
	             * simplification process_create_from_elf already made */

	/* checkpoint 12: a dynamically written file (mm/ramfs.c's own
	 * writable files -- see its header comment) takes priority over
	 * the fixed table here too, same reasoning as
	 * arch/risc/riscv64_syscall.c's sys_openat/sys_newfstatat -- this is
	 * what actually lets a freshly `tcc -o out.elf`'d binary be *run*,
	 * not just written: self-hosting.md's exit bar needs both, not
	 * just the write half. */
	struct ramfs_dynamic_file *dyn = ramfs_dynamic_lookup(path);
	const unsigned char *elf_data;
	unsigned long elf_data_size;
	if (dyn) {
		elf_data = dyn->data;
		elf_data_size = dyn->size;
	} else {
		const struct ramfs_file *file = ramfs_lookup(path);
		if (!file)
			return -1;
		elf_data = file->data;
		elf_data_size = file->size;
	}

	/* Snapshot argv strings into kernel memory *before* touching the
	 * address space they live in -- once the new address space is
	 * activated, `argv`/`argv[i]` (pointers into the *old* one) stop
	 * meaning anything. Bounded: nothing this kernel execve()s needs
	 * more than a handful of short arguments. */
	static char argbuf[PROCESS_EXEC_MAX_ARGS][EXECVE_ARG_MAX];
	int argc = 0;
	while (argv && argv[argc] && argc < PROCESS_EXEC_MAX_ARGS) {
		int i = 0;
		while (argv[argc][i] && i < EXECVE_ARG_MAX - 1) {
			argbuf[argc][i] = argv[argc][i];
			i++;
		}
		argbuf[argc][i] = 0;
		if (argv[argc][i])
			return -1;
		argc++;
	}
	if (argv && argv[argc])
		return -1;

	struct process *p = current_process;
	unsigned long *prev_root = paging_active_root(); /* == p->root_table, still valid until we succeed */
	unsigned long *new_root = paging_new_addrspace();
	paging_activate(new_root);

	unsigned long entry = elf_load(elf_data, elf_data_size);
	if (!entry) {
		paging_activate(prev_root);
		paging_destroy_addrspace(new_root);
		return -1;
	}

	/* argbuf's rows already are kernel-resident NUL-terminated strings
	 * (just snapshotted above) -- build_user_stack() just needs an
	 * actual array of pointers to them, not the 2D array itself (whose
	 * row stride, EXECVE_ARG_MAX, isn't the pointer-array layout it
	 * expects). */
	char *argv_kernel[PROCESS_EXEC_MAX_ARGS];
	for (int a = 0; a < argc; a++)
		argv_kernel[a] = argbuf[a];
	unsigned long sp = build_user_stack(p, argv_kernel, argc);

	p->root_table = new_root;
	paging_destroy_addrspace(prev_root);

	process_arch_execve_rewrite(r, entry, sp);

	return 0;
}
