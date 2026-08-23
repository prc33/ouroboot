#include "kernel.h"
#include "arch/idt.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/elf.h"
#include "sched/task.h"
#include "user_test_payload.h"
#include "hello_elf_payload.h"

#define MULTIBOOT_MAGIC 0x2BADB002u

struct multiboot_info {
	unsigned int flags;
	unsigned int mem_lower;
	unsigned int mem_upper;
};

static volatile unsigned int g_ticks = 0;
static volatile int g_breakpoint_hit = 0;

static void timer_handler(struct regs *r) {
	(void)r;
	g_ticks++;
}

static void breakpoint_handler(struct regs *r) {
	(void)r;
	g_breakpoint_hit = 1;
	kprintf("breakpoint: int3 handled and resumed OK\n");
}

/* --- COW test -------------------------------------------------------
 * One physical frame, mapped read-only+COW at two different virtual
 * addresses (standing in for "parent" and "child" views of a shared
 * page, since we don't have real per-process address spaces yet --
 * that's P5+). First write through each mapping should fault, copy,
 * and remap private+writable; the two should end up independent, and
 * the original frame (still reachable via its own identity mapping)
 * must be untouched by either write. */
#define VA_PARENT 0x400000u
#define VA_CHILD  0x401000u

static void run_cow_test(void) {
	unsigned int shared_phys = pmm_alloc_page();
	*(volatile int *)(unsigned long)shared_phys = 0xAAAA; /* via identity map, pre-COW */

	paging_map_page(VA_PARENT, shared_phys, PTE_PRESENT | PTE_COW);
	paging_map_page(VA_CHILD, shared_phys, PTE_PRESENT | PTE_COW);

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
 * A fully async forced-preemption switch is a documented follow-up
 * once serial output has its own locking. */
#define TICKS_PER_SWITCH 10
#define TOTAL_SWITCHES 6

static struct task task_a_ctx, task_b_ctx;
static volatile unsigned int g_switches = 0;

/* --- P5 checkpoint 1: ring3 + int 0x80 -------------------------------
 * No ELF loader yet (that's the next checkpoint) and no real fd/VFS
 * layer (SYS_write just forwards to the serial console) -- this test
 * isolates exactly one risk: does the ring0->ring3 transition, the
 * DPL=3 syscall gate, and the return path back through iret all work
 * correctly. The payload is hand-assembled machine code (see
 * user_test/user_test.S), embedded as static data since there's
 * nothing to load it from yet. */
static unsigned char kernel_stack[4096] __attribute__((aligned(16)));

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
 * user_test/hello.elf is the exact static musl+tcc binary from the
 * earlier compiler spike, embedded unmodified. Its syscall needs
 * (SYS_set_thread_area, SYS_set_tid_address, SYS_brk, SYS_mmap2,
 * SYS_munmap, SYS_ioctl, SYS_writev, SYS_exit_group) were derived by
 * running it under `qemu-i386-static -strace` first -- see
 * docs/kernel-p5-findings.md -- not guessed.
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
void run_elf_test(void) {
	unsigned int entry = elf_load(hello_elf_payload, HELLO_ELF_SIZE);
	if (!entry) {
		kprintf("FATAL: elf_load failed\n");
		for (;;) __asm__ volatile ("cli\n hlt");
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

void kmain(unsigned int magic, unsigned int mb_info_addr) {
	serial_init();

	kprintf("\n");
	kprintf("================================================\n");
	kprintf(" self-hosting-system kernel -- P4 checkpoint\n");
	kprintf("================================================\n");

	if (magic != MULTIBOOT_MAGIC) {
		kprintf("FATAL: bad multiboot magic: %x (expected %x)\n", magic, MULTIBOOT_MAGIC);
		goto halt;
	}
	kprintf("multiboot magic OK\n");

	struct multiboot_info *mbi = (struct multiboot_info *)(unsigned long)mb_info_addr;
	unsigned int mem_upper_kb = 0;
	if (mbi->flags & 0x1) {
		mem_upper_kb = mbi->mem_upper;
		kprintf("mem_lower = %u KB, mem_upper = %u KB\n", mbi->mem_lower, mem_upper_kb);
	} else {
		kprintf("FATAL: no memory info from multiboot\n");
		goto halt;
	}

	gdt_init();
	idt_init();
	pic_remap();
	for (int i = 0; i < 16; i++)
		pic_set_mask(i);

	isr_register_handler(3, breakpoint_handler);
	__asm__ volatile ("int $3");
	if (!g_breakpoint_hit) {
		kprintf("FATAL: breakpoint handler did not run\n");
		goto halt;
	}

	pmm_init(0x100000u + mem_upper_kb * 1024u, 0); /* phys_base=0 -- i386 RAM starts at physical 0 */
	paging_init(mem_upper_kb);

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

halt:
	kprintf("halting.\n");
	for (;;)
		__asm__ volatile ("hlt");
}
