/* The historical P4/P5 checkpoint chain -- kept in full as a
 * regression suite (kernel/Makefile's `test` target), but no longer
 * part of the product kernel. Compiled in only when KERNEL_CHECKPOINTS
 * is defined (arch/i386/kmain.c's own `kmain()` calls
 * run_checkpoint_boot() instead of booting straight to an interactive
 * shell -- see that file's comment). Mirrors
 * test/riscv64_checkpoints.c's own split exactly (checkpoint 20,
 * docs/repo-review-2026-08-26.md section 4).
 *
 * This is a straight move, not a rewrite: every function here is
 * exactly what arch/i386/kmain.c used to run unconditionally, on every
 * boot, including the product one -- see that file's own comment for
 * why that was real, load-bearing complexity in the wrong place
 * (checkpoint scaffolding making up 140 of kmain.c's 418 lines, plus a
 * 3,696-line embedded ELF hex dump that was 47% of the built i386
 * kernel.elf image, both now gone from every non-test build). The one
 * genuine change, not just a move: P5 checkpoint 2's real musl+TCC
 * binary is now user_test/hello_i386.c's built ELF, loaded from this
 * checkpoint's own initrd via required_initrd_file()+elf_load() --
 * exactly how test/riscv64_checkpoints.c's own P5 checkpoint 2 already
 * loads user_test/hello_riscv64.c's -- instead of a pre-built hex dump
 * with no live rebuild path. P5 checkpoint 1's payload stays an
 * embedded raw flat binary (user_test_payload.h, moved here unchanged
 * from arch/i386/): it was never an ELF (checkpoint 1 predates this
 * kernel having an ELF loader at all -- see run_ring3_test's own
 * comment), it's 18 lines, and there's no reason to invent an initrd
 * dependency for it. */
#include "../kernel.h"
#include "../arch/i386/idt.h"
#include "../mm/pmm.h"
#include "../mm/paging.h"
#include "../mm/elf.h"
#include "../mm/ramfs.h"
#include "../sched/task.h"
#include "../syscall_common.h"
#include "user_test_payload.h"

static struct ramfs_dynamic_file *required_initrd_file(const char *name) {
	struct ramfs_dynamic_file *f = ramfs_dynamic_lookup(name);
	if (!f)
		kprintf("FATAL: initrd is missing %s\n", name);
	return f;
}

/* --- COW test -- see arch/i386/kmain.c's git history for the original
 * comment (P5+ per-address-space paging superseded the need for this
 * kernel-only-pages demo, but it's still a real regression test of
 * arch/i386/paging.c's own COW mechanism in isolation). One physical
 * frame, mapped read-only+COW at two different virtual addresses,
 * standing in for "parent" and "child" views of a shared page. First
 * write through each mapping should fault, copy, and remap private+
 * writable; the two should end up independent, and the original frame
 * (still reachable via its own identity mapping) must be untouched by
 * either write. */
#define VA_PARENT 0x400000u
#define VA_CHILD  0x401000u

static void run_cow_test(void) {
	unsigned int shared_phys = pmm_alloc_page();
	*(volatile int *)(unsigned long)shared_phys = 0xAAAA; /* via identity map, pre-COW */

	paging_map_page(VA_PARENT, shared_phys, PTE_PRESENT | PTE_COW);
	pmm_retain_page(shared_phys);
	paging_map_page(VA_CHILD, shared_phys, PTE_PRESENT | PTE_COW);
	pmm_retain_page(shared_phys);

	unsigned int before = pmm_free_pages();

	*(volatile int *)VA_PARENT = 1; /* faults: copies, remaps VA_PARENT private */
	*(volatile int *)VA_CHILD = 2;  /* faults again: copies, remaps VA_CHILD private */

	unsigned int after = pmm_free_pages();
	int parent_val = *(volatile int *)VA_PARENT;
	int child_val = *(volatile int *)VA_CHILD;
	int original_val = *(volatile int *)(unsigned long)shared_phys;
	unsigned int parent_phys = paging_get_phys(VA_PARENT);
	unsigned int child_phys = paging_get_phys(VA_CHILD);

	int ok = (parent_val == 1) && (child_val == 2) && (original_val == 0xAAAA) &&
	         (parent_phys != shared_phys) && (child_phys != shared_phys) &&
	         (parent_phys != child_phys) && (before - after == 2);

	if (!ok) {
		kprintf("FATAL: COW test failed (parent=%d child=%d orig=%x "
			"parent_phys=%p child_phys=%p shared_phys=%p pages_used=%u)\n",
			parent_val, child_val, original_val,
			(void *)(unsigned long)parent_phys, (void *)(unsigned long)child_phys,
			(void *)(unsigned long)shared_phys, before - after);
		return;
	}
	kprintf("COW test: parent=%d child=%d OK\n", parent_val, child_val);
}

/* --- two-task scheduler test -----------------------------------------
 * Switches are cooperative, triggered at a safe point (top of each
 * task's loop) once enough timer ticks have passed -- not forced from
 * inside the timer ISR itself. Deliberate: kprintf/serial_putc isn't
 * reentrant-safe, so an async mid-instruction switch could interleave
 * two tasks' output mid-string and make this test's assertions
 * unreliable. The *cadence* is still genuinely timer-driven (g_ticks
 * comes from IRQ0); only the switch instant is a controlled point.
 * Superseded by the general process scheduler (sched/process.c) from
 * checkpoint 17 onward -- kept here, not in the product build, as a
 * standalone regression test of switch_context()'s save/restore
 * mechanism in isolation. */
#define TICKS_PER_SWITCH 10
#define TOTAL_SWITCHES 6

static struct task task_a_ctx, task_b_ctx;
static volatile unsigned int g_ticks = 0;
static volatile unsigned int g_switches = 0;

static void timer_handler(struct regs *r) {
	(void)r;
	g_ticks++;
}

/* --- P5 checkpoint 1: ring3 + int 0x80 -------------------------------
 * No ELF loader yet (that's the next checkpoint) and no real fd/VFS
 * layer (SYS_write just forwards to the serial console) -- this test
 * isolates exactly one risk: does the ring0->ring3 transition, the
 * DPL=3 syscall gate, and the return path back through iret all work
 * correctly. The payload is hand-assembled machine code
 * (user_test/user_test.S), embedded as static data since there's
 * nothing to load it from yet -- not a real ELF, so it doesn't belong
 * in the initrd the way P5 checkpoint 2's real binary does (see this
 * file's own header comment). */
static unsigned char kernel_stack[4096] __attribute__((aligned(16)));

static void run_elf_test(void); /* forward -- P5 checkpoint 1 chains into it */

/* Fires via syscall_posix.c's pre-process-mode exit hook -- see
 * process.h's own comment. Prints exactly what arch/i386/syscall.c's
 * sys_exit used to hardcode before checkpoint 18 moved that logic to
 * the generic syscall layer. */
static void checkpoint1_to_checkpoint2(void) {
	kprintf("ring3 test OK\n");
	kprintf("P5 checkpoint 1 OK\n");
	run_elf_test();
}

static void run_ring3_test(void) {
	syscall_init();
	tss_set_kernel_stack((unsigned int)(unsigned long)&kernel_stack[sizeof(kernel_stack)]);

	unsigned int payload_phys = pmm_alloc_page();
	unsigned char *dst = (unsigned char *)(unsigned long)payload_phys;
	for (unsigned int i = 0; i < USER_TEST_SIZE; i++)
		dst[i] = user_test_payload[i];
	paging_map_page(USER_TEST_LOAD_ADDR, payload_phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);

	unsigned int stack_phys = pmm_alloc_page();
	unsigned int user_stack_va = 0x900000u;
	paging_map_page(user_stack_va, stack_phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);

	syscall_set_pre_process_exit_hook(checkpoint1_to_checkpoint2);
	kprintf("ring3: entering userspace at %p...\n", (void *)(unsigned long)USER_TEST_ENTRY);
	enter_usermode(USER_TEST_ENTRY, user_stack_va + PAGE_SIZE);
	/* never reached: enter_usermode transitions permanently to ring3;
	 * the payload's exit syscall is what concludes the whole test run */
}

static void conclude_scheduler_test(void) {
	kprintf("scheduler: %u switches across 2 tasks OK\n", g_switches);
	kprintf("P4 checkpoint 2 OK\n");
	run_ring3_test();
}

/* --- P5 checkpoint 2: real ELF loader + real musl binary --------------
 * user_test/hello_i386.c's built ELF -- checkpoint 20 moved this from
 * a pre-built hex dump (arch/i386/hello_elf_payload.h, see this file's
 * own header comment) to a real initrd file, loaded the same way
 * test/riscv64_checkpoints.c's own P5 checkpoint 2 already does.
 *
 * Stack layout matches the Linux i386 ABI exactly (verified against
 * musl's own crt_arch.h: _start reads argc at *esp, argv at esp+4):
 *   [esp+0]  argc
 *   [esp+4]  argv[0] (pointer)
 *   [esp+8]  argv[1] = NULL
 *   [esp+12] envp[0] = NULL  (empty environment)
 *   [esp+16] auxv[0] = {AT_NULL, 0}
 * placed near the top of a 2-page stack, leaving room below for real
 * stack growth during musl's startup path and main(). */
static void finish_checkpoint_boot(void) {
	kprintf("ring3 test OK\n");
	kprintf("P5 checkpoint 2 OK\n");
	kprintf("halting.\n");
	arch_halt_forever();
}

static void run_elf_test(void) {
	struct ramfs_dynamic_file *file = required_initrd_file("hello");
	unsigned int entry = file ? elf_load(file->data, file->size) : 0;
	if (!entry) {
		kprintf("FATAL: elf_load failed\n");
		arch_halt_forever();
	}

	unsigned int stack_va = 0xB0000000u;
	unsigned int stack_pages = 2;
	for (unsigned int i = 0; i < stack_pages; i++) {
		unsigned int phys = pmm_alloc_page();
		paging_map_page(stack_va + i * PAGE_SIZE, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
	}
	unsigned int stack_top = stack_va + stack_pages * PAGE_SIZE;

	unsigned char *page = (unsigned char *)(unsigned long)stack_top;
	/* string data, 64 bytes below the very top of the mapped region */
	char *argv0 = (char *)(page - 64);
	const char *name = "hello";
	for (int i = 0; name[i]; i++)
		argv0[i] = name[i];
	argv0[5] = 0;

	/* argc/argv/envp/auxv block, 96 bytes below the top -- comfortably
	 * clear of the string data, leaves ~8000 bytes below for real
	 * stack use */
	unsigned int *sp = (unsigned int *)(page - 96);
	sp[0] = 1;                              /* argc */
	sp[1] = (unsigned int)(unsigned long)argv0; /* argv[0] */
	sp[2] = 0;                              /* argv[1] = NULL */
	sp[3] = 0;                              /* envp[0] = NULL */
	sp[4] = 0;                              /* auxv[0].a_type = AT_NULL */
	sp[5] = 0;                              /* auxv[0].a_val */

	syscall_set_pre_process_exit_hook(finish_checkpoint_boot);
	kprintf("elf: entering userspace at %p, esp=%p...\n",
		(void *)(unsigned long)entry, (void *)sp);
	enter_usermode(entry, (unsigned int)(unsigned long)sp);
	/* never reached */
}

static void task_body(char letter) {
	unsigned int last_tick = g_ticks;
	unsigned int my_loops = 0;
	for (;;) {
		while (g_ticks - last_tick < TICKS_PER_SWITCH)
			__asm__ volatile ("hlt");
		last_tick = g_ticks;
		my_loops++;
		g_switches++;
		kprintf("TASK %c: loop %u (switch %u/%u)\n", letter, my_loops, g_switches, TOTAL_SWITCHES);
		if (g_switches >= TOTAL_SWITCHES)
			conclude_scheduler_test();
		task_yield();
	}
}

static void task_a(void) {
	__asm__ volatile ("sti"); /* see task_init's comment: skipped on first launch */
	task_body('A');
}

static void task_b(void) {
	__asm__ volatile ("sti");
	task_body('B');
}

/* Entry point arch/i386/kmain.c's kmain() calls when built with
 * -DKERNEL_CHECKPOINTS, right after the same hardware/mm/fs bring-up
 * the product boot shares -- see that file's own comment. */
void run_checkpoint_boot(void) {
	run_cow_test();

	irq_register_handler(0, timer_handler);
	pit_init(100);
	pic_clear_mask(0);
	__asm__ volatile ("sti");

	task_init(&task_a_ctx, 0, task_a);
	task_init(&task_b_ctx, 1, task_b);
	task_register(0, &task_a_ctx);
	task_register(1, &task_b_ctx);

	kprintf("scheduler: starting two tasks...\n");
	task_start_scheduler(&task_a_ctx);
	/* never reached: task_start_scheduler does not return */
}
