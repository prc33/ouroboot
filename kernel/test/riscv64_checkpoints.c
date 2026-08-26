/* The historical P1-P10 checkpoint chain -- kept in full as a
 * regression suite (kernel/Makefile's `test`/`test-wasm` targets), but
 * no longer part of the product kernel. Compiled in only when
 * KERNEL_CHECKPOINTS is defined (riscv64_kmain.c's own `kmain()`
 * calls run_checkpoint_boot() instead of booting straight to an
 * interactive shell -- see that file's comment).
 *
 * This is a straight move, not a rewrite: every function here is
 * exactly what riscv64_kmain.c used to run unconditionally, on every
 * boot, including the product one -- see docs/kernel-complexity-review.md
 * section 1 for why that was real, load-bearing complexity in the
 * wrong place (arch/risc/riscv64_syscall.c's sys_exit/sys_exit_group used
 * to call run_elf_test()/run_process_test() directly, and
 * sched/riscv64_process.c's halt path printed "P10 checkpoint OK"
 * unconditionally -- both fixed generically, via hooks, rather than
 * by teaching the syscall/scheduler layers about checkpoints). */
#include "../kernel.h"
#include "../arch/risc/riscv64_trap.h"
#include "../mm/pmm.h"
#include "../mm/paging.h"
#include "../mm/elf.h"
#include "../mm/ramfs.h"
#include "../sched/task.h"
#include "../sched/process.h"

static struct ramfs_dynamic_file *required_initrd_file(const char *name) {
	struct ramfs_dynamic_file *f = ramfs_dynamic_lookup(name);
	if (!f)
		kprintf("FATAL: initrd is missing %s\n", name);
	return f;
}

/* --- COW test -- see kmain.c's run_cow_test for the full rationale;
 * this is the same test, riscv64-typed (unsigned long addresses). */
#define VA_PARENT 0x400000UL
#define VA_CHILD  0x401000UL

static void run_cow_test(void) {
	unsigned long shared_phys = pmm_alloc_page();
	*(volatile int *)shared_phys = 0xAAAA; /* via identity map, pre-COW */

	paging_map_page(VA_PARENT, shared_phys, PTE_PRESENT | PTE_COW);
	paging_map_page(VA_CHILD, shared_phys, PTE_PRESENT | PTE_COW);

	unsigned int before = pmm_free_pages();

	*(volatile int *)VA_PARENT = 1; /* faults: copies, remaps VA_PARENT private */
	*(volatile int *)VA_CHILD = 2;  /* faults again: copies, remaps VA_CHILD private */

	unsigned int after = pmm_free_pages();
	int parent_val = *(volatile int *)VA_PARENT;
	int child_val = *(volatile int *)VA_CHILD;
	int original_val = *(volatile int *)shared_phys;
	unsigned long parent_phys = paging_get_phys(VA_PARENT);
	unsigned long child_phys = paging_get_phys(VA_CHILD);

	int ok = (parent_val == 1) && (child_val == 2) && (original_val == 0xAAAA) &&
	         (parent_phys != shared_phys) && (child_phys != shared_phys) &&
	         (parent_phys != child_phys) && (before - after == 2);

	if (!ok) {
		kprintf("FATAL: COW test failed (parent=%d child=%d orig=%x "
			"parent_phys=%p child_phys=%p shared_phys=%p pages_used=%u)\n",
			parent_val, child_val, original_val,
			(void *)parent_phys, (void *)child_phys,
			(void *)shared_phys, before - after);
		return;
	}
	kprintf("COW test: parent=%d child=%d OK\n", parent_val, child_val);
}

/* Regression test for a real bug (arch/risc/riscv64_paging.c's
 * page_fault_handler, see its own comment): the COW-copy remap used
 * to hardcode PTE_PRESENT|PTE_WRITABLE, dropping PTE_USER entirely.
 * Invisible in run_cow_test() above -- it never sets PTE_USER on its
 * own pages to begin with (a purely kernel-side demo), so there was
 * nothing to lose. This test deliberately does map with PTE_USER, so
 * a regression here (the flag silently dropping again) is caught
 * directly by inspecting the post-copy PTE, not just indirectly by
 * checkpoint 7's fork() test eventually hanging/faulting on it. */
#define VA_USER_COW 0x402000UL

static void run_cow_user_test(void) {
	unsigned long shared_phys = pmm_alloc_page();
	*(volatile int *)shared_phys = 0x5555;

	paging_map_page(VA_USER_COW, shared_phys, PTE_PRESENT | PTE_USER | PTE_COW);

	*(volatile int *)VA_USER_COW = 99; /* faults: copies, remaps VA_USER_COW private */

	unsigned long flags = paging_get_flags(VA_USER_COW);
	int val = *(volatile int *)VA_USER_COW;

	if (!(flags & PTE_USER) || val != 99) {
		kprintf("FATAL: COW copy dropped PTE_USER or corrupted data (flags=%p val=%d)\n",
			(void *)flags, val);
		for (;;) __builtin_riscv_wfi();
	}
	kprintf("COW test: PTE_USER preserved after copy OK\n");
}

/* --- two-task scheduler test -- see kmain.c's equivalent for the
 * full rationale (cooperative switches at a safe point, not forced
 * mid-instruction from inside the timer IRQ itself, since kprintf/
 * serial_putc isn't reentrant-safe). The cadence is still genuinely
 * timer-driven (g_ticks comes from the Sstc tick handler).
 *
 * Superseded by the general process scheduler (sched/riscv64_process.c)
 * from checkpoint 6 onward -- kept here, not in the product build, as
 * a standalone regression test of switch_context()'s save/restore
 * mechanism in isolation, the same reasoning
 * docs/kernel-complexity-review.md section 1 gives for keeping it as
 * a test rather than deleting it outright. */
#define TICKS_PER_SWITCH 10
#define TOTAL_SWITCHES 6

static struct task task_a_ctx, task_b_ctx;
static volatile unsigned int g_ticks = 0;
static volatile unsigned int g_switches = 0;

static void timer_tick(void) {
	g_ticks++;
}

/* --- P5 checkpoint 1: ring3 + ecall -------------------------------
 * See kmain.c's run_ring3_test for the full rationale. No tss_set_
 * kernel_stack equivalent needed here: unlike i386 (which needs the
 * TSS to tell the CPU where the kernel stack is on a privilege-level
 * change), arch/risc/riscv64_trap_entry.S already unconditionally switches
 * to the same dedicated trap stack on *every* trap regardless of
 * origin -- one less thing this transition needs to set up. */
static void run_elf_test(void); /* forward -- P5 checkpoint 1 chains into it */

static void ring3_test_done(void) {
	/* Fires once, via arch/risc/riscv64_syscall.c's pre-process-mode exit
	 * hook -- see that file's own comment. Prints exactly what
	 * sys_exit()/sys_exit_impl() used to hardcode, now supplied by the
	 * test that actually cares about it instead of the syscall layer
	 * knowing checkpoint numbers. */
	kprintf("ring3 test OK\n");
	kprintf("P5 checkpoint 1 OK\n");
	run_elf_test();
}

static void run_ring3_test(void) {
	syscall_init();
	syscall_set_pre_process_exit_hook(ring3_test_done);
	struct ramfs_dynamic_file *file = required_initrd_file("user_test");
	unsigned long entry = file ? elf_load(file->data, file->size) : 0;
	if (!entry)
		for (;;) __builtin_riscv_wfi();

	unsigned long stack_phys = pmm_alloc_page();
	unsigned long user_stack_va = 0x900000UL;
	paging_map_page(user_stack_va, stack_phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);

	kprintf("ring3: entering userspace at %p...\n", (void *)entry);
	enter_usermode(entry, user_stack_va + PAGE_SIZE);
	/* never reached: enter_usermode transitions permanently to U-mode;
	 * the program's exit syscall is what concludes the whole test run */
}

/* --- P5 checkpoint 2: real ELF loader + real musl binary --------------
 * user_test/hello_riscv64.elf is a real static musl+TCC riscv64
 * binary. Stack layout matches the Linux riscv64 ABI (verified against
 * musl's own crt_arch.h/crt1.c: _start_c reads argc at s0[0], argv at
 * s0[1], where s0 == the incoming sp, captured via TCC's own frame-
 * pointer prologue convention rather than any assembly):
 *   [sp+0]  argc
 *   [sp+8]  argv[0] (pointer)
 *   [sp+16] argv[1] = NULL
 *   [sp+24] envp[0] = NULL  (empty environment)
 *   [sp+32] auxv[0] = {AT_NULL, 0}
 * placed near the top of a 2-page stack, same proportions as i386's
 * version, just 8-byte slots instead of 4-byte ones.
 *
 * NOTE: this is a separate, hand-rolled stack-builder, kept
 * deliberately unconverged with sched/riscv64_process.c's canonical
 * one -- see docs/kernel-complexity-review.md section 3's own
 * resolution for why: this test predates the process table (it runs
 * before process_init() has ever been called), and moving it onto the
 * shared builder would mean either running the whole process
 * subsystem earlier than this checkpoint is meant to demonstrate, or
 * leaving the shared builder able to run with no current_process at
 * all. Not worth it for a test whose entire point is "the loader and
 * ring3 transition work before there's a process table". */
static void run_process_test(void); /* forward -- P5 checkpoint 2 chains into it */

static void elf_test_done(void) {
	kprintf("ring3 test OK\n");
	kprintf("P5 checkpoint 2 OK\n");
	run_process_test();
}

static void run_elf_test(void) {
	struct ramfs_dynamic_file *file = required_initrd_file("hello");
	unsigned long entry = file ? elf_load(file->data, file->size) : 0;
	if (!entry) {
		kprintf("FATAL: elf_load failed\n");
		for (;;) __builtin_riscv_wfi();
	}

	unsigned long stack_va = 0xB0000000UL;
	unsigned long stack_pages = 2;
	for (unsigned long i = 0; i < stack_pages; i++) {
		unsigned long phys = pmm_alloc_page();
		paging_map_page(stack_va + i * PAGE_SIZE, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
	}
	unsigned long stack_top = stack_va + stack_pages * PAGE_SIZE;

	unsigned char *page = (unsigned char *)stack_top;
	char *argv0 = (char *)(page - 64);
	const char *name = "hello";
	for (int i = 0; name[i]; i++)
		argv0[i] = name[i];
	argv0[5] = 0;

	unsigned long *sp = (unsigned long *)(page - 128);
	sp[0] = 1;                       /* argc */
	sp[1] = (unsigned long)argv0;    /* argv[0] */
	sp[2] = 0;                       /* argv[1] = NULL */
	sp[3] = 0;                       /* envp[0] = NULL */
	sp[4] = 6;                       /* auxv[0].a_type = AT_PAGESZ */
	sp[5] = PAGE_SIZE;               /* auxv[0].a_val */
	sp[6] = 0;                       /* auxv[1].a_type = AT_NULL */
	sp[7] = 0;                       /* auxv[1].a_val */

	syscall_set_pre_process_exit_hook(elf_test_done);
	kprintf("elf: entering userspace at %p, sp=%p...\n", (void *)entry, (void *)sp);
	enter_usermode(entry, (unsigned long)sp);
	/* never reached */
}

/* --- checkpoint 6: general process table, two real independent
 * processes cooperatively scheduled across genuinely separate address
 * spaces -- see sched/riscv64_process.c and user_test/proc_test_riscv64.c
 * for the mechanism and the interleave signature this checks for. */
static void run_exec_test(void);
static void run_init_test(void);
static void run_interactive_test(void);

static struct process *process_from_initrd(const char *file_name, const char *process_name) {
	struct ramfs_dynamic_file *file = required_initrd_file(file_name);
	return file ? process_create_from_elf(file->data, file->size, process_name) : 0;
}

static void run_fork_test(void) {
	kprintf("P6 checkpoint OK\n");
	process_set_drain_hook(run_exec_test);
	struct process *p = process_from_initrd("proc_fork_test", "fork_test");
	if (!p) {
		kprintf("FATAL: process_create_from_elf failed\n");
		for (;;) __builtin_riscv_wfi();
	}
	kprintf("process: fork test process created (pid %d)\n", p->pid);
	process_run(p);
	/* not expected to return */
}

/* --- checkpoint 8: real ramfs (mm/ramfs.c) + open()/read()/close() +
 * execve() -- see user_test/proc_exec_test_riscv64.c and
 * user_test/exec_target_riscv64.c for the two real binaries involved
 * (fork()'s child execve()s from one into the completely separate
 * other one), and sched/riscv64_process.c's process_execve() for the
 * mechanism. Chained the same way as checkpoint 7 above. */
static void run_exec_test(void) {
	kprintf("P7 checkpoint OK\n");
	process_set_drain_hook(run_init_test);
	struct process *p = process_from_initrd("proc_exec_test", "exec_test");
	if (!p) {
		kprintf("FATAL: process_create_from_elf failed\n");
		for (;;) __builtin_riscv_wfi();
	}
	kprintf("process: exec test process created (pid %d)\n", p->pid);
	process_run(p);
	/* not expected to return */
}

/* --- checkpoint 9: the actual milestone -- real busybox ash, forked
 * and exec'd via mm/ramfs.h's multi-call table, running a real script
 * that exercises both an ash builtin and a real external command
 * (echo) resolved and exec'd through busybox's own argv[0] dispatch.
 * See user_test/init_test_riscv64.c and mm/ramfs.c's test.sh for the
 * full mechanism. Chained the same way as every checkpoint above. */
static void run_init_test(void) {
	kprintf("P8 checkpoint OK\n");
	process_set_drain_hook(run_interactive_test);
	struct process *p = process_from_initrd("init_test", "init_test");
	if (!p) {
		kprintf("FATAL: process_create_from_elf failed\n");
		for (;;) __builtin_riscv_wfi();
	}
	kprintf("process: init test process created (pid %d)\n", p->pid);
	process_run(p);
	/* not expected to return */
}

/* --- checkpoint 10: the "interactively" half of docs/emulator-plan.md's
 * P4 exit criterion -- real busybox ash (-i, forced interactive; see
 * user_test/interactive_test_riscv64.c) reading real commands from
 * real stdin (the UART) instead of a ramfs script file. This is the
 * last stage in the chain -- see finish_checkpoint_boot's own comment
 * for how it prints the closing "P10 checkpoint OK" without the
 * generic scheduler's halt path needing to know that. */
static void finish_checkpoint_boot(void) {
	/* Fires via process_set_drain_hook() below, once the process table
	 * has fully drained with nothing left to schedule (real ash
	 * exiting) -- the same generic mechanism every earlier stage in
	 * this chain already uses to hand off to the next one, just used
	 * here for "there is no next one" instead. sched/riscv64_process.c's
	 * own process_halt() is the ordinary, checkpoint-agnostic halt path
	 * every real boot (product included) reaches the same way once its
	 * own last process exits. */
	kprintf("P10 checkpoint OK\n");
	process_halt();
}

static void run_interactive_test(void) {
	kprintf("P9 checkpoint OK\n");
	process_set_drain_hook(finish_checkpoint_boot);
	struct process *p = process_from_initrd("interactive_test", "interactive_test");
	if (!p) {
		kprintf("FATAL: process_create_from_elf failed\n");
		for (;;) __builtin_riscv_wfi();
	}
	kprintf("process: interactive test process created (pid %d)\n", p->pid);
	process_run(p);
	/* not expected to return */
}

static void run_process_test(void) {
	process_init();
	process_set_drain_hook(run_fork_test);
	struct process *a = process_from_initrd("proc_test", "A");
	struct process *b = process_from_initrd("proc_test", "B");
	if (!a || !b) {
		kprintf("FATAL: process_create_from_elf failed\n");
		for (;;) __builtin_riscv_wfi();
	}
	kprintf("process: two independent processes created (pid %d, pid %d)\n", a->pid, b->pid);
	process_run(a);
	/* not expected to return: process_exit_current() chains into
	 * run_fork_test() once both processes above have exited, then
	 * halts for real once *that* test's process exits too */
}

static void conclude_scheduler_test(void) {
	kprintf("scheduler: %u switches across 2 tasks OK\n", g_switches);
	kprintf("P4 checkpoint 2 OK\n");
	/* This was the only thing the timer was ever for -- see
	 * timer_disable()'s own comment for the real bug leaving it armed
	 * caused (a nested-trap corruption only paced, real-world-timed
	 * input could actually trigger, so no scripted/all-at-once test
	 * ever exercised it). */
	timer_disable();
	run_ring3_test();
}

static void task_body(char letter) {
	unsigned int last_tick = g_ticks;
	unsigned int my_loops = 0;
	for (;;) {
		while (g_ticks - last_tick < TICKS_PER_SWITCH)
			__builtin_riscv_wfi();
		last_tick = g_ticks;
		my_loops++;
		g_switches++;
		kprintf("TASK %c: loop %u (switch %u/%u)\n", letter, my_loops, g_switches, TOTAL_SWITCHES);
		if (g_switches >= TOTAL_SWITCHES)
			conclude_scheduler_test();
		task_yield();
	}
}

static void task_a(void) { task_body('A'); }
static void task_b(void) { task_body('B'); }

/* Entry point riscv64_kmain.c's kmain() calls when built with
 * -DKERNEL_CHECKPOINTS, right after the same hardware/mm/fs bring-up
 * the product boot shares -- see that file's own comment. */
void run_checkpoint_boot(void) {
	run_cow_test();
	run_cow_user_test();

	timer_set_tick_handler(timer_tick);
	timer_init(100);

	task_init(&task_a_ctx, 0, task_a);
	task_init(&task_b_ctx, 1, task_b);
	task_register(0, &task_a_ctx);
	task_register(1, &task_b_ctx);

	kprintf("scheduler: starting two tasks...\n");
	task_start_scheduler(&task_a_ctx);
	/* never reached: task_start_scheduler does not return */
}
