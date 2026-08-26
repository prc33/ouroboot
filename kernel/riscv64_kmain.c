#include "kernel.h"
#include "arch/riscv64_trap.h"
#include "arch/riscv64_memmap.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/elf.h"
#include "mm/ramfs.h"
#include "mm/tar.h"
#include "sched/process.h"

static volatile int g_breakpoint_hit = 0;

static struct ramfs_dynamic_file *required_initrd_file(const char *name) {
	struct ramfs_dynamic_file *f = ramfs_dynamic_lookup(name);
	if (!f)
		kprintf("FATAL: initrd is missing %s\n", name);
	return f;
}

static void breakpoint_handler(struct regs *r) {
	g_breakpoint_hit = 1;
	r->sepc += 4; /* our codegen never emits compressed (2-byte) instructions, so ebreak is always 4 bytes; skip past it or we'd loop on it forever */
	kprintf("breakpoint: ebreak handled and resumed OK\n");
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
 * memory, and every later boot path needs the same free pool.
 *
 * Kept unconditional (unlike the P1-P10 checkpoint chain --
 * kernel/test/riscv64_checkpoints.c, see this file's own comment on
 * why that moved out) because it is a real, load-bearing self-check
 * of the memory allocator every boot -- product included -- actually
 * depends on, not a demonstration of superseded mechanism: if pmm
 * ever again hands out a page inside the scratch region, every boot
 * corrupts silently, in a way nothing later would reliably catch. It
 * is also cheap (a few hundred page allocations, immediately freed)
 * and adds no state or coupling the rest of the kernel has to know
 * about. */
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

#ifdef KERNEL_CHECKPOINTS
/* kernel/test/riscv64_checkpoints.c -- the full historical P1-P10
 * regression chain, compiled in only for kernel/Makefile's
 * `test`/`test-wasm` targets. Not declared in kernel.h: nothing
 * outside this file and the checkpoint file itself needs to know it
 * exists, which is the whole point (see docs/kernel-complexity-review.md
 * section 1). */
void run_checkpoint_boot(void);
#endif

/* Real boot: bring up hardware/memory/the filesystem, then hand off
 * straight to an interactive shell -- no historical checkpoint chain,
 * no demonstration of superseded mechanism (the P4 fixed-2-task
 * scheduler, ring3-before-there-was-a-process-table, an ELF loaded
 * outside the process table). Every one of those still exists and
 * still runs, just in kernel/test/riscv64_checkpoints.c under
 * -DKERNEL_CHECKPOINTS (kernel/Makefile's `test`/`test-wasm`) rather
 * than on every boot including this one -- see that file's own
 * comment, and docs/kernel-complexity-review.md sections 1-2 for why
 * this split exists and how the syscall/scheduler layers were
 * decoupled from checkpoint numbering to make it possible. */
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
	/* Reserved up front, same reasoning as the scratch region right
	 * above, so nothing else can be handed this memory before
	 * tar_load_initrd() below gets to read whatever's actually there. */
	pmm_reserve_range((unsigned int)RV64_INITRD_BASE, (unsigned int)(RV64_INITRD_BASE + RV64_INITRD_MAX_SIZE));
	run_pmm_reserve_test();
	paging_init(RV64_MEM_TOP);

	/* The filesystem is always an explicit boot module; kernel.elf
	 * contains no ramdisk bytes. */
	unsigned int initrd_files = tar_load_initrd((const unsigned char *)RV64_INITRD_BASE, RV64_INITRD_MAX_SIZE);
	kprintf("initrd: %u file(s) loaded from tar at %p\n", initrd_files, (void *)RV64_INITRD_BASE);
	if (!initrd_files) {
		kprintf("FATAL: an explicit initrd tar is required\n");
		goto halt;
	}

#ifdef KERNEL_CHECKPOINTS
	run_checkpoint_boot();
	/* never reached: chains all the way through to
	 * kernel/test/riscv64_checkpoints.c's own finish_checkpoint_boot(),
	 * which halts */
#else
	syscall_init();
	process_init();
	/* checkpoint 15: real argv (argc=2, {"ash","-i"}) straight against
	 * BusyBox's own ELF -- no separate wrapper binary needed (there
	 * used to be one, "interactive_test", whose only job was
	 * execve("ash",["ash","-i"]), because process_create_from_elf()'s
	 * older single-arg0 stack builder couldn't carry a second argv
	 * element). See sched/riscv64_process.c's
	 * process_create_from_elf_argv() and docs/kernel-complexity-review.md
	 * section 3. */
	struct ramfs_dynamic_file *busybox_elf = required_initrd_file("busybox");
	char *ash_argv[] = { "ash", "-i", 0 };
	struct process *init = busybox_elf ? process_create_from_elf_argv(busybox_elf->data, busybox_elf->size, ash_argv, 2) : 0;
	if (!init) {
		kprintf("FATAL: process_create_from_elf_argv failed\n");
		goto halt;
	}
	kprintf("process: init process created (pid %d)\n", init->pid);
	process_run(init);
	/* never reached: process_run() only returns via process_halt(),
	 * once the process table drains with nothing left to schedule --
	 * see sched/riscv64_process.c's own comment */
#endif

halt:
	kprintf("halting.\n");
	for (;;)
		__builtin_riscv_wfi();
}
