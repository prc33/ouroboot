#include "kernel.h"
#include "arch/riscv64_trap.h"
#include "arch/riscv64_memmap.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/elf.h"
#include "mm/tar.h"
#include "sched/task.h"
#include "sched/process.h"
#include "user_test_riscv64_payload.h"
#include "hello_elf_riscv64_payload.h"
#include "proc_test_elf_riscv64_payload.h"
#include "proc_fork_test_elf_riscv64_payload.h"
#include "proc_exec_test_elf_riscv64_payload.h"
#include "init_test_elf_riscv64_payload.h"
#include "interactive_test_elf_riscv64_payload.h"

static volatile int g_breakpoint_hit = 0;

static void breakpoint_handler(struct regs *r) {
	g_breakpoint_hit = 1;
	r->sepc += 4; /* our codegen never emits compressed (2-byte) instructions, so ebreak is always 4 bytes; skip past it or we'd loop on it forever */
	kprintf("breakpoint: ebreak handled and resumed OK\n");
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

/* Regression test for a real bug (mm/riscv64_paging.c's
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

/* Regression test for a real bug (mm/pmm.h's pmm_reserve_range(), see
 * its own comment): pmm_init() never reserved
 * arch/riscv64_memmap.h's hardcoded scratch region (boot stack, trap
 * dispatch pointer, trapframe, trap stack), so pmm_alloc_page() could
 * -- and, once enough allocations happened, did -- hand out a page
 * underneath the kernel's own currently-running boot stack. Directly
 * exercises the actual failure mode: allocate enough pages to reach
 * past the scratch region (same order of magnitude that surfaced the
 * real bug), and assert none of them fall inside it. Frees everything
 * back afterward -- this runs before anything else has claimed real
 * memory, and every later checkpoint needs the same free pool. */
#define PMM_RESERVE_TEST_COUNT 300

static void run_pmm_reserve_test(void) {
	static unsigned int addrs[PMM_RESERVE_TEST_COUNT];
	unsigned int n;
	int hit_reserved = 0;

	for (n = 0; n < PMM_RESERVE_TEST_COUNT; n++) {
		addrs[n] = pmm_alloc_page();
		if (!addrs[n])
			break; /* genuinely out of memory before hitting the count -- fine, just stop */
		if (addrs[n] >= (unsigned int)RV64_SCRATCH_BASE && addrs[n] < (unsigned int)RV64_TRAP_STACK_TOP)
			hit_reserved = 1;
	}
	for (unsigned int i = 0; i < n; i++)
		pmm_free_page(addrs[i]);

	if (hit_reserved) {
		kprintf("FATAL: pmm handed out a page inside the reserved scratch region\n");
		for (;;) __builtin_riscv_wfi();
	}
	kprintf("pmm: reserve test OK (%u pages, none in [%p, %p))\n",
		n, (void *)RV64_SCRATCH_BASE, (void *)RV64_TRAP_STACK_TOP);
}

/* --- two-task scheduler test -- see kmain.c's equivalent for the
 * full rationale (cooperative switches at a safe point, not forced
 * mid-instruction from inside the timer IRQ itself, since kprintf/
 * serial_putc isn't reentrant-safe). The cadence is still genuinely
 * timer-driven (g_ticks comes from the Sstc tick handler).
 *
 * Unlike i386, task_a/task_b need no explicit "re-enable interrupts"
 * step on first launch: sstatus.SIE is a single global CPU-wide flag
 * here, enabled once by timer_init() below and never saved/restored
 * per task (sched/riscv64_switch_context.S is plain register-only,
 * sstatus-agnostic) -- i386 needs it because EFLAGS.IF is restored
 * implicitly by `iret` on every *normal* resume, which a task's very
 * first launch (a plain switch_context `ret`, not an iret) skips; on
 * riscv64 there's no per-task interrupt-enable state to skip
 * restoring in the first place. */
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
 * change), arch/riscv64_trap_entry.S already unconditionally switches
 * to the same dedicated trap stack on *every* trap regardless of
 * origin -- one less thing this transition needs to set up. */
static unsigned char kernel_stack[4096] __attribute__((aligned(16)));

static void run_ring3_test(void) {
	(void)kernel_stack; /* no per-task kernel stack needed -- see comment above; kept for shape parity with kmain.c */
	syscall_init();

	unsigned int npages = (USER_TEST_RISCV64_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
	for (unsigned int i = 0; i < npages; i++) {
		unsigned long phys = pmm_alloc_page();
		paging_map_page(USER_TEST_RISCV64_LOAD_ADDR + i * PAGE_SIZE, phys,
			PTE_PRESENT | PTE_WRITABLE | PTE_USER);
		unsigned char *dst = (unsigned char *)phys;
		for (unsigned int j = 0; j < PAGE_SIZE; j++) {
			unsigned int off = i * PAGE_SIZE + j;
			dst[j] = off < USER_TEST_RISCV64_SIZE ? user_test_riscv64_payload[off] : 0;
		}
	}

	unsigned long stack_phys = pmm_alloc_page();
	unsigned long user_stack_va = 0x900000UL;
	paging_map_page(user_stack_va, stack_phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);

	kprintf("ring3: entering userspace at %p...\n", (void *)USER_TEST_RISCV64_ENTRY);
	enter_usermode(USER_TEST_RISCV64_ENTRY, user_stack_va + PAGE_SIZE);
	/* never reached: enter_usermode transitions permanently to U-mode;
	 * the payload's exit syscall is what concludes the whole test run */
}

/* --- P5 checkpoint 2: real ELF loader + real musl binary --------------
 * user_test/hello_riscv64.elf is a real static musl+TCC riscv64
 * binary (same role as i386's user_test/hello.elf -- see
 * docs/riscv-port-findings.md for the musl+TCC pipeline it's built
 * with). Stack layout matches the Linux riscv64 ABI (verified against
 * musl's own crt_arch.h/crt1.c: _start_c reads argc at s0[0], argv at
 * s0[1], where s0 == the incoming sp, captured via TCC's own frame-
 * pointer prologue convention rather than any assembly):
 *   [sp+0]  argc
 *   [sp+8]  argv[0] (pointer)
 *   [sp+16] argv[1] = NULL
 *   [sp+24] envp[0] = NULL  (empty environment)
 *   [sp+32] auxv[0] = {AT_NULL, 0}
 * placed near the top of a 2-page stack, same proportions as i386's
 * version, just 8-byte slots instead of 4-byte ones. */
void run_elf_test(void) {
	unsigned long entry = elf_load(hello_elf_riscv64_payload, HELLO_ELF_RISCV64_SIZE);
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
	/* string data, 64 bytes below the very top of the mapped region */
	char *argv0 = (char *)(page - 64);
	const char *name = "hello";
	for (int i = 0; name[i]; i++)
		argv0[i] = name[i];
	argv0[5] = 0;

	/* argc/argv/envp/auxv block, 128 bytes below the top -- comfortably
	 * clear of the string data, leaves ~8000 bytes below for real
	 * stack use, and lands 16-byte aligned (RISC-V ABI requirement at
	 * process entry) since 128 is a multiple of 16.
	 *
	 * AT_PAGESZ is not optional the way i386's kmain.c's single
	 * AT_NULL entry might suggest: musl's __libc_start_main does
	 * `libc.page_size = aux[AT_PAGESZ]` with no fallback if it's
	 * absent, so omitting it left page_size silently 0, corrupting
	 * mallocng's internal size math -- malloc() returned NULL, used
	 * unchecked by the test program, which then wrote through it.
	 * Found by comparing against a real `qemu-riscv64-static -strace`
	 * of this exact binary: entirely different (and far more
	 * plausible) syscall arguments than what actually ran under our
	 * kernel, pointing straight at the environment we hand it being
	 * the thing that diverged, not the syscalls' implementations. */
	unsigned long *sp = (unsigned long *)(page - 128);
	sp[0] = 1;                       /* argc */
	sp[1] = (unsigned long)argv0;    /* argv[0] */
	sp[2] = 0;                       /* argv[1] = NULL */
	sp[3] = 0;                       /* envp[0] = NULL */
	sp[4] = 6;                       /* auxv[0].a_type = AT_PAGESZ */
	sp[5] = PAGE_SIZE;               /* auxv[0].a_val */
	sp[6] = 0;                       /* auxv[1].a_type = AT_NULL */
	sp[7] = 0;                       /* auxv[1].a_val */

	kprintf("elf: entering userspace at %p, sp=%p...\n", (void *)entry, (void *)sp);
	enter_usermode(entry, (unsigned long)sp);
	/* never reached */
}

/* --- checkpoint 6: general process table, two real independent
 * processes cooperatively scheduled across genuinely separate address
 * spaces -- see sched/riscv64_process.c and user_test/proc_test_riscv64.c
 * for the mechanism and the interleave signature this checks for. */
/* --- checkpoint 7: real fork()+wait4() -- see
 * user_test/proc_fork_test_riscv64.c and sched/riscv64_process.c's
 * process_fork/process_wait4 for the mechanism. Chained in via
 * process_set_drain_hook() below, run *after* checkpoint 6's two
 * processes both finish -- see sched/riscv64_process.c's drain_hook
 * comment for why that's the right place to hook a new test's setup
 * in, same idea as arch/riscv64_syscall.c's sys_exit/sys_exit_group
 * chaining P4->P5 checkpoint 1->P5 checkpoint 2. */
static void run_exec_test(void);
static void run_init_test(void);
static void run_interactive_test(void);

static void run_fork_test(void) {
	kprintf("P6 checkpoint OK\n");
	process_set_drain_hook(run_exec_test);
	struct process *p = process_create_from_elf(proc_fork_test_elf_riscv64_payload, PROC_FORK_TEST_ELF_RISCV64_SIZE, "fork_test");
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
	struct process *p = process_create_from_elf(proc_exec_test_elf_riscv64_payload, PROC_EXEC_TEST_ELF_RISCV64_SIZE, "exec_test");
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
	struct process *p = process_create_from_elf(init_test_elf_riscv64_payload, INIT_TEST_ELF_RISCV64_SIZE, "init_test");
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
 * real stdin (the UART) instead of a ramfs script file. What actually
 * feeds those commands in lives outside the kernel entirely: real
 * QEMU via kernel/test/boot_test.py's --stdin-input (-serial stdio
 * maps straight onto the process's own stdin), and emulator/js/'s own
 * --input (uart.js's pushInput(), there since P3, exercised for real
 * for the first time here) -- see kernel/Makefile for exactly what
 * gets typed. */
static void run_interactive_test(void) {
	kprintf("P9 checkpoint OK\n");
	struct process *p = process_create_from_elf(interactive_test_elf_riscv64_payload, INTERACTIVE_TEST_ELF_RISCV64_SIZE, "interactive_test");
	if (!p) {
		kprintf("FATAL: process_create_from_elf failed\n");
		for (;;) __builtin_riscv_wfi();
	}
	kprintf("process: interactive test process created (pid %d)\n", p->pid);
	process_run(p);
	/* not expected to return */
}

void run_process_test(void) {
	process_init();
	process_set_drain_hook(run_fork_test);
	struct process *a = process_create_from_elf(proc_test_elf_riscv64_payload, PROC_TEST_ELF_RISCV64_SIZE, "A");
	struct process *b = process_create_from_elf(proc_test_elf_riscv64_payload, PROC_TEST_ELF_RISCV64_SIZE, "B");
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

void kmain(unsigned long hartid, unsigned long dtb) {
	(void)hartid;
	(void)dtb;
	serial_init();

	kprintf("\n");
	kprintf("================================================\n");
	kprintf(" self-hosting-system kernel -- riscv64\n");
	kprintf("================================================\n");
	kprintf("boot: entered kmain in S-mode\n");

	trap_init();

	isr_register_handler(3, breakpoint_handler); /* scause 3 = Breakpoint */
	__builtin_riscv_ebreak();
	if (!g_breakpoint_hit) {
		kprintf("FATAL: breakpoint handler did not run\n");
		goto halt;
	}

	pmm_init((unsigned int)RV64_MEM_TOP, (unsigned int)RV64_RAM_BASE);
	/* arch/riscv64_memmap.h's hardcoded scratch region (boot stack,
	 * trap dispatch pointer, trapframe, trap stack) isn't part of the
	 * kernel image pmm_init() already excludes -- see
	 * mm/pmm.h's pmm_reserve_range() comment for why this is required,
	 * not defensive. */
	pmm_reserve_range((unsigned int)RV64_SCRATCH_BASE, (unsigned int)RV64_TRAP_STACK_TOP);
	/* checkpoint 13: arch/riscv64_memmap.h's own comment -- reserved
	 * up front, same reasoning as the scratch region right above, so
	 * nothing else can be handed this memory before tar_load_initrd()
	 * below gets to read whatever's actually there. */
	pmm_reserve_range((unsigned int)RV64_INITRD_BASE, (unsigned int)(RV64_INITRD_BASE + RV64_INITRD_MAX_SIZE));
	run_pmm_reserve_test();
	paging_init(RV64_MEM_TOP);

	/* checkpoint 13: a real, separate boot module -- see mm/tar.h's
	 * own comment. Harmless (0 files, real memory that was never
	 * written to reads as zero, so the tar parser's own end-of-archive
	 * check fires on the very first header) if nothing actually loaded
	 * one -- every existing checkpoint below runs identically either
	 * way, only kernel/Makefile's own test-initrd target actually
	 * supplies a real archive. */
	unsigned int initrd_files = tar_load_initrd((const unsigned char *)RV64_INITRD_BASE, RV64_INITRD_MAX_SIZE);
	kprintf("initrd: %u file(s) loaded from tar at %p\n", initrd_files, (void *)RV64_INITRD_BASE);

#ifdef KERNEL_DIRECT_SHELL
	/* Product/performance boot: the checkpoint chain remains the default test
	 * build, but a user waiting for ash should not have to execute every
	 * historical milestone first. Use the same process and exec path as P10. */
	syscall_init();
	process_init();
	run_interactive_test();
#endif

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

halt:
	kprintf("halting.\n");
	for (;;)
		__builtin_riscv_wfi();
}
