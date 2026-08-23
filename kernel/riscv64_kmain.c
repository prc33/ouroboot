#include "kernel.h"
#include "arch/riscv64_trap.h"

static volatile int g_breakpoint_hit = 0;

static void breakpoint_handler(struct regs *r) {
	g_breakpoint_hit = 1;
	r->sepc += 4; /* our codegen never emits compressed (2-byte) instructions, so ebreak is always 4 bytes; skip past it or we'd loop on it forever */
	kprintf("breakpoint: ebreak handled and resumed OK\n");
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

halt:
	kprintf("halting.\n");
	for (;;)
		__builtin_riscv_wfi();
}
