/* Standard non-PAE x86 paging: two-level page directory + page tables,
 * 4KB pages throughout (not 4MB/PSE -- COW needs per-page control).
 *
 * Design simplification, worth stating explicitly: the whole address
 * space this kernel manages is identity-mapped (virt == phys for
 * every page ever handed out via pmm_alloc_page). That means a
 * physical address is always safely dereferenceable as a pointer,
 * paging on or off, so there's no separate phys->virt translation
 * function anywhere in this file -- same simplification
 * arch/risc/riscv64_paging.c states for itself.
 *
 * Checkpoint 17 (docs/kernel-arch-split-plan.md, "genericize rather
 * than write afresh"): this used to be the whole story -- one fixed
 * page directory, one fixed pool of page tables, no per-process
 * address space at all. Brought up to riscv64_paging.c's own shape
 * instead of inventing a separate one: dynamic table allocation via
 * pmm_alloc_page() (alloc_table(), same technique, same reason --
 * scales with however many processes actually exist instead of a pool
 * sized for exactly one address space), paging_new_addrspace()/
 * paging_activate()/the _in() accessor variants, and the actual
 * COW-copy/page-fault-dispatch logic now shared outright via
 * mm/paging_common.c (this file only decodes CR2/the error code and
 * hands them to paging_handle_fault() -- see that file's own comment
 * for why it doesn't need to know anything about this two-level walk
 * at all). `root_table` below is "the kernel's own address space",
 * same role riscv64_paging.c's own root_table plays. */
#include "kernel.h"
#include "idt.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "sched/process.h"

#define ENTRIES 1024

static unsigned long root_table[ENTRIES] __attribute__((aligned(4096)));

/* Whichever address space get_pte()/paging_map_page()/paging_get_phys()/
 * the page fault handler operate on when no explicit root is given --
 * i.e. what CR3 actually holds right now. paging_activate() is the
 * only thing that changes it, always in lockstep with the real
 * register write. Defaults to the kernel's own table (set at the
 * bottom of paging_init()) -- same convention and same reason as
 * arch/risc/riscv64_paging.c's own active_root. */
static unsigned long *active_root = 0;

/* The range every new address space shares 1:1 with the kernel's own
 * table (paging_new_addrspace(), below) -- top_addr from paging_init(),
 * rounded up to the next 4MB (one page-directory entry) boundary. */
static unsigned long kernel_pd_top;

/* Real per-process addresses (mm/elf.c-loaded ELF code/data, sched/process.c's
 * user stack at 0xB0000000+, its mmap arena at 0x60000000+) must never
 * land inside the shared region above, or every process would silently
 * alias the same page tables for its own private memory -- exactly the
 * bug arch/risc/riscv64_paging.c's own paging_new_addrspace() comment
 * describes hitting for real at checkpoint 6. Confirmed safe here by
 * measurement, not assumption (docs/kernel-arch-split-plan.md): the
 * real musl+TCC ELF test binary (P5 checkpoint 2) loads at exactly
 * 0x08048198 -- 128MB plus a few hundred bytes, the first real address
 * *past* this cap. Matches mm/pmm.c's own MAX_MEMORY_MB (128u) --
 * there's no real RAM above this bound to identity-map or share in the
 * first place, on either side of that comparison. */
#define MAX_SHARED_MB 128u

static inline void invlpg(unsigned int addr) {
	__asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

static inline void load_cr3(unsigned int addr) {
	__asm__ volatile ("movl %0, %%cr3" : : "r"(addr) : "memory");
}

static inline unsigned int read_cr2(void) {
	unsigned int v;
	__asm__ volatile ("movl %%cr2, %0" : "=r"(v));
	return v;
}

static inline void enable_paging(void) {
	/* CR0.PG (bit 31) turns paging on. CR0.WP (bit 16) makes the CPU
	 * actually enforce the PTE writable bit against ring-0 writes --
	 * without it, supervisor-mode code silently ignores R/W=0 and COW
	 * does nothing at all (confirmed the hard way, back when this
	 * kernel's first COW test existed: it wrote straight through a
	 * "read-only" page with no fault, because WP defaults to 0). */
	__asm__ volatile (
		"movl %%cr0, %%eax\n"
		"orl $0x80010000, %%eax\n"
		"movl %%eax, %%cr0\n"
		: : : "eax"
	);
}

/* Table pages come from the general physical allocator, not a fixed
 * pool sized for exactly one address space -- see this file's own
 * comment and arch/risc/riscv64_paging.c's identical alloc_table() for
 * why. */
static unsigned long *alloc_table(void) {
	unsigned long phys = pmm_alloc_page();
	if (!phys) {
		kprintf("FATAL: i386 paging out of memory allocating a page table\n");
		for (;;) __asm__ volatile ("cli\n hlt");
	}
	unsigned long *t = (unsigned long *)phys; /* identity-mapped -- see file comment */
	for (int i = 0; i < ENTRIES; i++)
		t[i] = 0;
	return t;
}

/* Walks (creating the page table as needed, if `create`) down to the
 * PTE covering `virt` within address space `root`, and returns a
 * pointer to that PTE slot itself. */
static unsigned long *get_pte_in(unsigned long *root, unsigned long virt, int create) {
	unsigned int pd_idx = (virt >> 22) & 0x3FFu;
	unsigned long *table;
	if (root[pd_idx] & PTE_PRESENT) {
		table = (unsigned long *)(root[pd_idx] & ~0xFFFUL);
	} else {
		if (!create)
			return 0;
		table = alloc_table();
		/* x86 ANDs the permission bits from the PDE and the PTE -- a
		 * page only ends up user-accessible/writable if BOTH say so.
		 * The real restriction lives at the leaf PTE (that's how COW
		 * marks a page read-only, and how kernel-only pages stay
		 * non-user), so every PDE is made maximally permissive here
		 * rather than tracked per-table. Real bug this comment is
		 * inherited from (this kernel's first ring3 test, long before
		 * per-address-space support existed): PTE said USER, a PDE
		 * created earlier for a kernel-only mapping didn't -- present=1
		 * but the access still faulted, since the PDE's missing USER
		 * bit vetoed it. */
		root[pd_idx] = ((unsigned long)table) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
	}
	unsigned int pt_idx = (virt >> 12) & 0x3FFu;
	return &table[pt_idx];
}

static unsigned long *get_pte(unsigned long virt, int create) {
	return get_pte_in(active_root, virt, create);
}

void paging_map_page_in(unsigned long *root, unsigned long virt, unsigned long phys, unsigned long flags) {
	unsigned long *pte = get_pte_in(root, virt, 1);
	*pte = (phys & ~0xFFFUL) | flags;
	if (root == active_root)
		invlpg((unsigned int)virt);
}

void paging_map_page(unsigned long virt, unsigned long phys, unsigned long flags) {
	paging_map_page_in(active_root, virt, phys, flags);
}

unsigned long paging_get_phys_in(unsigned long *root, unsigned long virt) {
	unsigned long *pte = get_pte_in(root, virt, 0);
	if (!pte || !(*pte & PTE_PRESENT))
		return 0;
	return (*pte & ~0xFFFUL) | (virt & 0xFFFUL);
}

unsigned long paging_get_phys(unsigned long virt) {
	return paging_get_phys_in(active_root, virt);
}

unsigned long paging_get_flags_in(unsigned long *root, unsigned long virt) {
	unsigned long *pte = get_pte_in(root, virt, 0);
	if (!pte || !(*pte & PTE_PRESENT))
		return 0;
	return *pte & 0xFFFUL;
}

unsigned long paging_get_flags(unsigned long virt) {
	return paging_get_flags_in(active_root, virt);
}

/* Switches to address space `root`: writes CR3 and updates active_root
 * together so the two can never disagree -- same convention as
 * arch/risc/riscv64_paging.c's own paging_activate(). A CR3 reload
 * flushes the whole TLB by itself (no PG_GLOBAL pages exist in this
 * kernel), which is also exactly what paging_flush_tlb() below relies
 * on. */
void paging_activate(unsigned long *root) {
	active_root = root;
	load_cr3((unsigned int)(unsigned long)root);
}

unsigned long *paging_active_root(void) {
	return active_root;
}

/* i386 has no single-instruction "flush everything" the way riscv64's
 * sfence.vma is -- invlpg only ever covers one page. Reloading CR3
 * with its own current value re-flushes the entire TLB in one shot,
 * which is all mm/paging_common.c's paging_fork_cow() actually needs
 * (it's already established that src_root/dst_root is the active one
 * before calling this). */
void paging_flush_tlb(void) {
	load_cr3((unsigned int)(unsigned long)active_root);
}

/* A fresh address space for a new process: its own page directory,
 * with every PDE covering [0, kernel_pd_top) copied straight from the
 * kernel's own root_table -- the whole 4MB window each entry covers,
 * not just the specific sub-range something's actually mapped, unlike
 * riscv64_paging.c's own paging_new_addrspace() (which shares at a
 * *finer* granularity than its identity-mapped region specifically
 * because 1GB-slot sharing collided with real per-process addresses
 * there). i386's page-directory-entry granularity is already 4MB, and
 * this file's own MAX_SHARED_MB comment is the measurement that
 * confirms whole-4MB-window sharing doesn't have riscv64's problem
 * here: no real per-process address this kernel ever hands out falls
 * below that cap. Sharing the *table pointer* itself (not just
 * re-mapping the same physical pages into a new table) means kernel
 * code/data edits made *after* a process is created -- there are
 * none, in practice, but riscv64's version relies on the same property
 * -- stay visible to every address space, same as S-mode always
 * seeing current kernel code regardless of which process's satp/CR3
 * is loaded. */
unsigned long *paging_new_addrspace(void) {
	unsigned long *root = alloc_table();
	unsigned int shared_entries = (unsigned int)(kernel_pd_top >> 22);
	if (shared_entries > ENTRIES)
		shared_entries = ENTRIES;
	for (unsigned int i = 0; i < shared_entries; i++)
		root[i] = root_table[i];
	return root;
}

/* Thin trap-decoding wrapper -- the actual COW-copy and page-fault
 * dispatch logic is mm/paging_common.c's paging_handle_fault(), shared
 * with riscv64 outright (see that file's own comment for why it
 * doesn't need anything about this two-level walk at all). */
static void page_fault_handler(struct regs *r) {
	unsigned int fault_addr = read_cr2();
	int present = r->err_code & 0x1;
	int is_write = (r->err_code & 0x2) != 0;
	int from_user = (r->err_code & 0x4) != 0;

	if (paging_handle_fault(fault_addr, is_write, from_user) == PAGE_FAULT_FIXED)
		return; /* isr epilogue does iret; faulting instruction retries and succeeds */

	kprintf("\n!! PAGE FAULT at %p (err=%x: %s %s %s)\n", (void *)(unsigned long)fault_addr,
		r->err_code,
		present ? "present" : "not-present",
		is_write ? "write" : "read",
		from_user ? "user" : "kernel");
	kprintf("FATAL: unhandled page fault, eip=%p pid=%d esp=%p\n", (void *)(unsigned long)r->eip,
		process_current_pid(), (void *)(unsigned long)r->useresp);
	for (;;) __asm__ volatile ("cli\n hlt");
}

void paging_init(unsigned long mem_upper_kb) {
	for (int i = 0; i < ENTRIES; i++)
		root_table[i] = 0;
	active_root = root_table;

	unsigned int top_addr = 0x100000u + (unsigned int)mem_upper_kb * 1024u;
	unsigned int max_identity = MAX_SHARED_MB * 1024u * 1024u;
	if (top_addr > max_identity)
		top_addr = max_identity;

	/* identity map [0, top_addr) 4KB at a time -- includes the low 1MB
	 * (BIOS area etc) even though pmm never hands those pages out, so
	 * the kernel itself can still read/write them if it ever needs to */
	for (unsigned int addr = 0; addr < top_addr; addr += PAGE_SIZE)
		paging_map_page(addr, addr, PTE_PRESENT | PTE_WRITABLE);

	/* Round up to a whole page-directory entry (4MB) -- paging_new_addrspace()
	 * shares whole PDEs, so the shared range must never stop mid-PDE
	 * (either leaking a fragment of *unmapped* kernel space into every
	 * process, if rounded down, or -- rounded up past MAX_SHARED_MB --
	 * sharing a PDE this loop never actually populated). */
	kernel_pd_top = ((unsigned long)top_addr + (4UL * 1024 * 1024 - 1)) & ~(4UL * 1024 * 1024 - 1);

	isr_register_handler(14, page_fault_handler);

	load_cr3((unsigned int)(unsigned long)root_table);
	enable_paging();

	kprintf("paging: identity-mapped 0..%p, page fault handler installed\n",
		(void *)(unsigned long)top_addr);
}
