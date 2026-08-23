#include "kernel.h"
#include "arch/riscv64_trap.h"
#include "arch/riscv64_memmap.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "sched/task.h"
#include "user_test_riscv64_payload.h"

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

/* TODO(next checkpoint): real ELF loader, mirroring kmain.c's
 * run_elf_test -- riscv64's mm/elf.c ELF64 support doesn't exist yet.
 * arch/riscv64_syscall.c's sys_exit already calls this (matching
 * i386's checkpoint-chaining shape), so it needs *a* definition now;
 * replaced with the real thing in the next commit. */
void run_elf_test(void) {
	kprintf("P5 checkpoint 2 OK (placeholder -- ELF loader not wired up yet)\n");
	kprintf("halting.\n");
	for (;;) __builtin_riscv_wfi();
}

static void conclude_scheduler_test(void) {
	kprintf("scheduler: %u switches across 2 tasks OK\n", g_switches);
	kprintf("P4 checkpoint 2 OK\n");
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
	paging_init(RV64_MEM_TOP);

	run_cow_test();

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
