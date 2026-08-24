/* Standard non-PAE x86 paging: one page directory, a pool of page
 * tables, 4KB pages throughout (not 4MB/PSE -- COW needs per-page
 * control, so keep everything uniform).
 *
 * Design simplification, worth stating explicitly: the whole address
 * space this kernel manages is identity-mapped (virt == phys for
 * every page we ever hand out via pmm_alloc_page). That means a
 * physical address is always safely dereferenceable as a pointer,
 * paging on or off, so there's no separate phys->virt translation
 * function anywhere in this file. This is fine for a single address
 * space; a real per-process address space (P5+) will need its own
 * mapping story on top of this, not instead of it -- physical pages
 * still get identity-mapped in kernel space for the kernel's own use
 * (copying between address spaces, etc), even once userspace pages
 * stop being identity-mapped 1:1 with their virtual addresses. */
#include "kernel.h"
#include "arch/idt.h"
#include "pmm.h"
#include "paging.h"

#define ENTRIES 1024
#define MAX_TABLES 32 /* 32 * 4MB = 128MB, matches pmm's MAX_MEMORY_MB */

static unsigned int page_directory[ENTRIES] __attribute__((aligned(4096)));
static unsigned int page_tables[MAX_TABLES][ENTRIES] __attribute__((aligned(4096)));
static int table_used[MAX_TABLES];

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
	 * without it, supervisor-mode code silently ignores R/W=0 and
	 * COW does nothing at all (confirmed the hard way: first attempt
	 * at the COW test below wrote straight through a "read-only"
	 * page with no fault, because WP defaults to 0). */
	__asm__ volatile (
		"movl %%cr0, %%eax\n"
		"orl $0x80010000, %%eax\n"
		"movl %%eax, %%cr0\n"
		: : : "eax"
	);
}

/* Finds (or lazily allocates) the page table covering `virt`, using a
 * fixed pool rather than pmm_alloc_page() for the table itself -- the
 * pool is simpler to reason about here and small enough (128MB worth
 * of tables = 128KB) not to matter. */
static unsigned int *get_table_for(unsigned int virt, int create) {
	unsigned int pd_idx = virt >> 22;
	if (page_directory[pd_idx] & PTE_PRESENT)
		return page_tables[pd_idx];
	if (!create)
		return 0;

	for (unsigned int i = 0; i < ENTRIES; i++)
		page_tables[pd_idx][i] = 0;
	/* x86 ANDs the permission bits from the PDE and the PTE -- a page
	 * only ends up user-accessible if BOTH say so. The real
	 * restriction lives at the PTE level (kernel identity-mapped
	 * pages simply never get PTE_USER on their PTE), so it's both
	 * standard practice and simpler to make every PDE permissive by
	 * default rather than trying to track "does this table end up
	 * holding any user mappings" here. First real ring3 test caught
	 * this the hard way: PTE said USER, PDE (created earlier, for a
	 * kernel-only mapping) didn't -- present=1 but the access still
	 * faulted, since the PDE's missing USER bit vetoed it. */
	page_directory[pd_idx] =
		((unsigned int)(unsigned long)&page_tables[pd_idx]) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
	table_used[pd_idx] = 1;
	return page_tables[pd_idx];
}

void paging_map_page(unsigned int virt, unsigned int phys, unsigned int flags) {
	unsigned int *table = get_table_for(virt, 1);
	unsigned int pt_idx = (virt >> 12) & 0x3FF;
	table[pt_idx] = (phys & ~0xFFFu) | flags;
	invlpg(virt);
}

unsigned int paging_get_phys(unsigned int virt) {
	unsigned int *table = get_table_for(virt, 0);
	if (!table)
		return 0;
	unsigned int pt_idx = (virt >> 12) & 0x3FF;
	if (!(table[pt_idx] & PTE_PRESENT))
		return 0;
	return (table[pt_idx] & ~0xFFFu) | (virt & 0xFFFu);
}

static void page_fault_handler(struct regs *r) {
	unsigned int fault_addr = read_cr2();
	unsigned int page = fault_addr & ~0xFFFu;
	int present = r->err_code & 0x1;
	int is_write = r->err_code & 0x2;

	unsigned int *table = get_table_for(page, 0);
	unsigned int pt_idx = (page >> 12) & 0x3FF;

	if (present && is_write && table && (table[pt_idx] & PTE_PRESENT) &&
	    (table[pt_idx] & PTE_COW)) {
		unsigned int old_phys = table[pt_idx] & ~0xFFFu;
		unsigned int new_phys = pmm_alloc_page();
		if (!new_phys) {
			kprintf("FATAL: page fault COW handler out of memory\n");
			goto fatal;
		}
		unsigned char *src = (unsigned char *)(unsigned long)old_phys;
		unsigned char *dst = (unsigned char *)(unsigned long)new_phys;
		for (unsigned int i = 0; i < PAGE_SIZE; i++)
			dst[i] = src[i];

		/* Preserve PTE_USER from the pre-copy PTE (still in
		 * table[pt_idx], not yet overwritten) rather than hardcoding
		 * just PRESENT|WRITABLE -- mirrors a real bug found and fixed
		 * on the riscv64 side (mm/riscv64_paging.c's page_fault_handler,
		 * see its comment) via a real fork()+wait4() test: the only
		 * COW exercised on i386 so far is kmain.c's own run_cow_test,
		 * entirely kernel-only pages with no PTE_USER to begin with,
		 * so this never actually manifested here -- but the bug is
		 * the same latent one, not a different one, so fixed the same
		 * way rather than left for whenever i386 gets its own real
		 * multi-process fork(). */
		table[pt_idx] = (new_phys & ~0xFFFu) | PTE_PRESENT | PTE_WRITABLE |
			(table[pt_idx] & PTE_USER);
		invlpg(page);
		return; /* isr epilogue does iret; faulting instruction retries and succeeds */
	}

fatal:
	kprintf("\n!! PAGE FAULT at %p (err=%x: %s %s %s)\n", (void *)(unsigned long)fault_addr,
		r->err_code,
		present ? "present" : "not-present",
		is_write ? "write" : "read",
		(r->err_code & 0x4) ? "user" : "kernel");
	kprintf("FATAL: unhandled page fault, eip=%p\n", (void *)(unsigned long)r->eip);
	for (;;) __asm__ volatile ("cli\n hlt");
}

void paging_init(unsigned int mem_upper_kb) {
	for (int i = 0; i < ENTRIES; i++)
		page_directory[i] = 0;

	unsigned int top_addr = 0x100000u + mem_upper_kb * 1024u;
	unsigned int max_identity = (unsigned int)MAX_TABLES * 4u * 1024u * 1024u;
	if (top_addr > max_identity)
		top_addr = max_identity;

	/* identity map [0, top_addr) 4KB at a time -- includes the low 1MB
	 * (BIOS area etc) even though pmm never hands those pages out, so
	 * the kernel itself can still read/write them if it ever needs to */
	for (unsigned int addr = 0; addr < top_addr; addr += PAGE_SIZE)
		paging_map_page(addr, addr, PTE_PRESENT | PTE_WRITABLE);

	isr_register_handler(14, page_fault_handler);

	load_cr3((unsigned int)(unsigned long)&page_directory);
	enable_paging();

	kprintf("paging: identity-mapped 0..%p, page fault handler installed\n",
		(void *)(unsigned long)top_addr);
}
