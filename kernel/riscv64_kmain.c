#include "kernel.h"
#include "arch/riscv64_trap.h"
#include "arch/riscv64_memmap.h"
#include "mm/pmm.h"
#include "mm/paging.h"

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

halt:
	kprintf("halting.\n");
	for (;;)
		__builtin_riscv_wfi();
}
