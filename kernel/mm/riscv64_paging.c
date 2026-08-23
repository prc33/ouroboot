/* Sv39 paging: 3-level radix tree, 9+9+9 bits of VPN plus a 12-bit
 * page offset, 4KB pages throughout. Same design simplification as
 * i386's mm/paging.c, stated there and equally true here: the whole
 * address space this kernel manages is identity-mapped, so there's
 * no separate phys->virt translation function anywhere in this file.
 *
 * Genuinely simpler than i386's two-level scheme in one respect:
 * RISC-V only checks R/W/X/U permission bits at the *leaf* PTE --
 * intermediate (non-leaf) PTEs just need V=1 to be walked through, no
 * i386-style "PDE must ALSO carry the USER bit or the PTE's USER bit
 * is vetoed" AND-ing to replicate.
 */
#include "kernel.h"
#include "arch/riscv64_trap.h"
#include "pmm.h"
#include "paging.h"

#define ENTRIES 512
#define VPN_MASK 0x1FFUL
#define PPN_SHIFT 10

#define CSR_SATP 0x180

/* Sv39 satp.MODE = 8, in the top 4 bits of the 64-bit CSR. */
#define SATP_MODE_SV39 (8UL << 60)

static unsigned long root_table[ENTRIES] __attribute__((aligned(4096)));

/* Pool for level-1/level-0 tables, same "fixed pool, not pmm_alloc_page,
 * simpler to reason about and small enough not to matter" choice as
 * i386's page_tables[MAX_TABLES][ENTRIES] -- worst case here is one
 * level-1 table plus one level-0 table per 2MB of mapped range;
 * 128MB / 2MB = 64, so 128 is comfortable headroom. */
#define MAX_TABLES 128
static unsigned long pool[MAX_TABLES][ENTRIES] __attribute__((aligned(4096)));
static int pool_used;

static unsigned long *alloc_table(void) {
	if (pool_used >= MAX_TABLES) {
		kprintf("FATAL: riscv64 paging table pool exhausted\n");
		for (;;) __builtin_riscv_wfi();
	}
	unsigned long *t = pool[pool_used++];
	for (int i = 0; i < ENTRIES; i++)
		t[i] = 0;
	return t;
}

/* Walks (creating intermediate tables as needed, if `create`) down to
 * the level-0 PTE covering `virt`, and returns a pointer to that PTE
 * slot itself -- caller reads/writes *pte directly, same shape as
 * i386's get_table_for()+indexing pattern. */
static unsigned long *get_pte(unsigned long virt, int create) {
	unsigned long *table = root_table;
	for (int level = 2; level >= 1; level--) {
		unsigned int idx = (virt >> (12 + 9 * level)) & VPN_MASK;
		if (!(table[idx] & 1)) { /* V bit -- intermediate PTEs only need this, not R/W/X */
			if (!create)
				return 0;
			unsigned long *next = alloc_table();
			table[idx] = (((unsigned long)next >> 12) << PPN_SHIFT) | 1; /* V only -- non-leaf */
		}
		table = (unsigned long *)((table[idx] >> PPN_SHIFT) << 12);
	}
	unsigned int idx0 = (virt >> 12) & VPN_MASK;
	return &table[idx0];
}

void paging_map_page(unsigned long virt, unsigned long phys, unsigned long flags) {
	unsigned long *pte = get_pte(virt, 1);
	*pte = ((phys >> 12) << PPN_SHIFT) | flags;
	__builtin_riscv_sfence_vma();
}

unsigned long paging_get_phys(unsigned long virt) {
	unsigned long *pte = get_pte(virt, 0);
	if (!pte || !(*pte & 1))
		return 0;
	return ((*pte >> PPN_SHIFT) << 12) | (virt & 0xFFFUL);
}

static void page_fault_handler(struct regs *r) {
	unsigned long fault_addr = r->stval;
	unsigned long page = fault_addr & ~0xFFFUL;
	int is_write = (r->scause == 15); /* Store/AMO page fault */

	unsigned long *pte = get_pte(page, 0);

	if (is_write && pte && (*pte & 1) && (*pte & PTE_COW)) {
		unsigned long old_phys = (*pte >> PPN_SHIFT) << 12;
		unsigned long new_phys = pmm_alloc_page();
		if (!new_phys) {
			kprintf("FATAL: page fault COW handler out of memory\n");
			goto fatal;
		}
		unsigned char *src = (unsigned char *)old_phys;
		unsigned char *dst = (unsigned char *)new_phys;
		for (unsigned int i = 0; i < PAGE_SIZE; i++)
			dst[i] = src[i];

		*pte = ((new_phys >> 12) << PPN_SHIFT) | PTE_PRESENT | PTE_WRITABLE;
		__builtin_riscv_sfence_vma();
		return; /* sret retries the faulting instruction, which now succeeds */
	}

fatal:
	kprintf("\n!! PAGE FAULT at %p (scause=%lu stval=%p %s)\n", (void *)fault_addr,
		r->scause, (void *)fault_addr, is_write ? "write" : "read/exec");
	kprintf("FATAL: unhandled page fault, sepc=%p\n", (void *)r->sepc);
	for (;;) __builtin_riscv_wfi();
}

void paging_init(unsigned long mem_top) {
	for (int i = 0; i < ENTRIES; i++)
		root_table[i] = 0;

	/* identity map [RV64_RAM_BASE, mem_top) 4KB at a time -- see
	 * mm/pmm.c's phys_base for why RAM doesn't start at 0 here. */
	for (unsigned long addr = 0x80000000UL; addr < mem_top; addr += PAGE_SIZE)
		paging_map_page(addr, addr, PTE_PRESENT | PTE_WRITABLE);

	/* UART0 (drivers/riscv64_serial.c) is MMIO, far outside RAM --
	 * unlike i386's port-I/O serial console (a completely separate
	 * address space paging can't affect at all), every kprintf/
	 * serial_putc call on riscv64 is a real memory access that goes
	 * through the page table once this is enabled. Forgetting this
	 * mapping was this port's next bug after boot/riscv64_boot.S's:
	 * satp got written correctly, but the very first kprintf
	 * afterward (reaching for the UART) instantly page-faulted --
	 * and the fault handler's own kprintf faulted the same way,
	 * silently, forever, with no output at all to say why. */
	paging_map_page(0x10000000UL, 0x10000000UL, PTE_PRESENT | PTE_WRITABLE);

	isr_register_handler(12, page_fault_handler); /* Instruction page fault */
	isr_register_handler(13, page_fault_handler); /* Load page fault */
	isr_register_handler(15, page_fault_handler); /* Store/AMO page fault */

	unsigned long satp = SATP_MODE_SV39 | ((unsigned long)root_table >> 12);
	__builtin_riscv_csrw(CSR_SATP, satp);
	__builtin_riscv_sfence_vma();

	kprintf("paging: Sv39 identity-mapped 0x80000000..%p, page fault handler installed\n",
		(void *)mem_top);
}
