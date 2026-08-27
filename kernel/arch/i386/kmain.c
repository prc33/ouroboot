#include "kernel.h"
#include "idt.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/elf.h"
#include "syscall_common.h"
#include "sched/process.h"
#include "mm/tar.h"
#include "mm/ramfs.h"

#define MULTIBOOT_MAGIC 0x2BADB002u

/* Real layout (Multiboot v1 spec), not just the 3 fields the original
 * P4 checkpoint needed -- checkpoint 19 needs mods_count/mods_addr
 * too, to find the initrd QEMU's own `-initrd <file>` flag loads as a
 * boot module. Every field up to mods_addr is unconditionally present
 * at these fixed offsets regardless of which `flags` bits are set --
 * only the *meaning* of "is this field's value valid" is flags-gated
 * (mem_lower/mem_upper need bit 0; mods_count/mods_addr need bit 3),
 * not the struct layout itself. */
struct multiboot_info {
	unsigned int flags;
	unsigned int mem_lower;
	unsigned int mem_upper;
	unsigned int boot_device;
	unsigned int cmdline;
	unsigned int mods_count;
	unsigned int mods_addr;
};

/* One entry in the mods_addr array above -- also fixed Multiboot v1
 * layout. `string` (a module command-line, e.g. what follows `-initrd`
 * on QEMU's own command line) is unused here: this kernel only ever
 * expects exactly one module and doesn't need to distinguish it by
 * name. */
struct multiboot_module {
	unsigned int mod_start;
	unsigned int mod_end;
	unsigned int string;
	unsigned int reserved;
};

static volatile int g_breakpoint_hit = 0;

static void breakpoint_handler(struct regs *r) {
	(void)r;
	g_breakpoint_hit = 1;
	kprintf("breakpoint: int3 handled and resumed OK\n");
}

static struct ramfs_dynamic_file *required_initrd_file(const char *name) {
	struct ramfs_dynamic_file *f = ramfs_dynamic_lookup(name);
	if (!f)
		kprintf("FATAL: initrd is missing %s\n", name);
	return f;
}

void arch_halt_forever(void) {
	for (;;) __asm__ volatile ("cli\n hlt");
}

#ifdef KERNEL_CHECKPOINTS
/* test/i386_checkpoints.c -- the historical P4/P5 checkpoint chain,
 * compiled in only for kernel/Makefile's `test` target. Not declared
 * in kernel.h: nothing outside this file and the checkpoint file
 * itself needs to know it exists -- see that file's own comment for
 * why this split exists, and docs/repo-review-2026-08-26.md section 4
 * for the measurement that motivated it. */
void run_checkpoint_boot(void);
#endif

/* Real boot: bring up hardware/memory/the filesystem, then hand off
 * straight to an interactive shell -- no historical checkpoint chain,
 * no demonstration of superseded mechanism (the P4 fixed-2-task
 * scheduler, ring3-before-there-was-a-process-table, an ELF loaded
 * outside the process table). Every one of those still exists and
 * still runs, just in test/i386_checkpoints.c under -DKERNEL_CHECKPOINTS
 * (kernel/Makefile's `test`) rather than on every boot including this
 * one -- see that file's own comment, and this file's git history
 * (checkpoint 20) for how the syscall/scheduler layers were already
 * decoupled from checkpoint numbering (checkpoint 18) to make this
 * split possible, and what the split saved: 140 of this file's
 * previous 418 lines were checkpoint scaffolding, plus a 3,696-line
 * embedded ELF hex dump that was 47% of the built i386 kernel.elf
 * image -- neither exists in this file or this build any more. */
void kmain(unsigned int magic, unsigned int mb_info_addr) {
	serial_init();

	kprintf("\n");
	kprintf("================================================\n");
	kprintf(" self-hosting-system kernel -- i386\n");
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

	/* The initrd QEMU's `-initrd <file>` loads as a Multiboot module --
	 * flags bit 3 (0x8) means mods_count/mods_addr are valid. Only the
	 * first module is used; this kernel never asks for more than one. */
	unsigned int initrd_base = 0, initrd_size = 0;
	if ((mbi->flags & 0x8) && mbi->mods_count >= 1) {
		struct multiboot_module *mod = (struct multiboot_module *)(unsigned long)mbi->mods_addr;
		initrd_base = mod->mod_start;
		initrd_size = mod->mod_end - mod->mod_start;
		kprintf("multiboot: initrd module at %p, %u bytes\n",
			(void *)(unsigned long)initrd_base, initrd_size);
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
	/* Reserved before anything else can be handed this memory -- same
	 * reasoning as arch/risc/riscv64_kmain.c's own initrd reservation,
	 * right after pmm_init(). QEMU's `-kernel`/Multiboot loader places
	 * module bytes wherever it likes, typically right after the kernel
	 * image -- pmm_init() only ever excludes [phys_base, kernel_end),
	 * so without this a later pmm_alloc_page() could eventually hand
	 * out a page still holding unread initrd bytes. */
	if (initrd_size)
		pmm_reserve_range(initrd_base, initrd_base + initrd_size);
	paging_init(mem_upper_kb);

	/* The filesystem is always an explicit boot module; kernel.elf
	 * contains no ramdisk bytes. */
	if (!initrd_size) {
		kprintf("FATAL: an explicit initrd is required (QEMU -initrd)\n");
		goto halt;
	}
	unsigned int initrd_files = tar_load_initrd((const unsigned char *)(unsigned long)initrd_base, initrd_size);
	kprintf("initrd: %u file(s) loaded from tar at %p\n", initrd_files, (void *)(unsigned long)initrd_base);
	if (!initrd_files) {
		kprintf("FATAL: initrd tar contained no files\n");
		goto halt;
	}

#ifdef KERNEL_CHECKPOINTS
	run_checkpoint_boot();
	/* never reached: chains all the way through to
	 * test/i386_checkpoints.c's own finish_checkpoint_boot(), which
	 * halts */
#else
	syscall_init();
	process_init();
	/* Real argv (argc=2, {"ash","-i"}) straight against BusyBox's own
	 * ELF -- see sched/process.c's process_create_from_elf_argv() and
	 * docs/kernel-complexity-review.md section 3 for why no separate
	 * wrapper binary is needed (same mechanism riscv64's own product
	 * boot uses). */
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
	 * see sched/process.c's own comment */
#endif

halt:
	kprintf("halting.\n");
	for (;;)
		__asm__ volatile ("hlt");
}
