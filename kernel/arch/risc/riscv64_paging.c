/* Sv39 paging: 3-level radix tree, 9+9+9 bits of VPN plus a 12-bit
 * page offset, 4KB pages throughout. Same design simplification as
 * i386's arch/i386/paging.c, stated there and equally true here: the whole
 * address space this kernel manages is identity-mapped, so there's
 * no separate phys->virt translation function anywhere in this file.
 *
 * Genuinely simpler than i386's two-level scheme in one respect:
 * RISC-V only checks R/W/X/U permission bits at the *leaf* PTE --
 * intermediate (non-leaf) PTEs just need V=1 to be walked through, no
 * i386-style "PDE must ALSO carry the USER bit or the PTE's USER bit
 * is vetoed" AND-ing to replicate.
 *
 * Checkpoint 6 (sched/riscv64_process.c) change: P1-P5 only ever had
 * one address space, `root_table` below, used implicitly everywhere.
 * Real processes need their own -- paging_new_addrspace()/
 * paging_activate() and the _in() variants of the original API are
 * new; `root_table` itself is now just "the kernel's own address
 * space" (still what every *_in-less call operates on until something
 * calls paging_activate(), and still what every new address space's
 * kernel-region mapping is shared from -- see paging_new_addrspace()).
 *
 * Checkpoint 17 (docs/kernel-arch-split-plan.md): the actual COW-copy
 * logic (paging_fork_cow(), the page-fault dispatch, paging_ensure_writable())
 * moved out to mm/paging_common.c -- it turned out not to need
 * anything about the walk shape at all, just the accessor functions
 * this file still provides (get_pte_in() and everything built on it
 * stay right here, genuinely riscv64-specific). See that file's own
 * comment for the full rationale.
 */
#include "kernel.h"
#include "riscv64_trap.h"
#include "riscv64_memmap.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "sched/process.h"

#define ENTRIES 512
#define VPN_MASK 0x1FFUL
#define PPN_SHIFT 10

#define CSR_SATP 0x180
#define SSTATUS_SPP (1UL << 8)

/* Sv39 satp.MODE = 8, in the top 4 bits of the 64-bit CSR. */
#define SATP_MODE_SV39 (8UL << 60)

static unsigned long root_table[ENTRIES] __attribute__((aligned(4096)));

/* Whichever address space get_pte()/paging_map_page()/paging_get_phys()/
 * the page fault handler operate on when no explicit root is given --
 * i.e. what satp actually holds right now. paging_activate() is the
 * only thing that changes it, always in lockstep with the real CSR
 * write, so this is never stale. Defaults to the kernel's own table
 * (set at the bottom of paging_init()), matching every P1-P5 call
 * site's assumption that there's just the one address space. */
static unsigned long *active_root = 0;

/* Table pages for intermediate (level 2/1) PTEs now come from the
 * general physical allocator instead of a fixed pool sized for
 * exactly one address space -- P1-P5's `pool[MAX_TABLES][ENTRIES]`
 * static array assumed there'd only ever be one address space's worth
 * of tables to hold; a real per-process address space per fork/exec
 * needs this to scale with however many processes actually exist.
 * pmm_alloc_page() already exists and is exactly this kind of general
 * allocator -- reusing it instead of inventing a second one. */
static unsigned long *alloc_table(void) {
	unsigned long phys = pmm_alloc_page();
	if (!phys) {
		kprintf("FATAL: riscv64 paging out of memory allocating a page table\n");
		for (;;) __builtin_riscv_wfi();
	}
	unsigned long *t = (unsigned long *)phys; /* identity-mapped -- see file comment */
	for (int i = 0; i < ENTRIES; i++)
		t[i] = 0;
	return t;
}

/* Walks (creating intermediate tables as needed, if `create`) down to
 * the level-0 PTE covering `virt` within address space `root`, and
 * returns a pointer to that PTE slot itself -- caller reads/writes
 * *pte directly, same shape as i386's get_table_for()+indexing
 * pattern. */
static unsigned long *get_pte_in(unsigned long *root, unsigned long virt, int create) {
	unsigned long *table = root;
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

static unsigned long *get_pte(unsigned long virt, int create) {
	return get_pte_in(active_root, virt, create);
}

void paging_map_page_in(unsigned long *root, unsigned long virt, unsigned long phys, unsigned long flags) {
	unsigned long *pte = get_pte_in(root, virt, 1);
	*pte = ((phys >> 12) << PPN_SHIFT) | flags;
	if (root == active_root)
		__builtin_riscv_sfence_vma();
}

void paging_map_page(unsigned long virt, unsigned long phys, unsigned long flags) {
	paging_map_page_in(active_root, virt, phys, flags);
}

unsigned long paging_get_phys_in(unsigned long *root, unsigned long virt) {
	unsigned long *pte = get_pte_in(root, virt, 0);
	if (!pte || !(*pte & 1))
		return 0;
	return ((*pte >> PPN_SHIFT) << 12) | (virt & 0xFFFUL);
}

unsigned long paging_get_phys(unsigned long virt) {
	return paging_get_phys_in(active_root, virt);
}

unsigned long paging_get_flags_in(unsigned long *root, unsigned long virt) {
	unsigned long *pte = get_pte_in(root, virt, 0);
	if (!pte || !(*pte & 1))
		return 0;
	return *pte & 0x3FFUL;
}

unsigned long paging_get_flags(unsigned long virt) {
	return paging_get_flags_in(active_root, virt);
}

void paging_flush_tlb(void) {
	__builtin_riscv_sfence_vma();
}

/* Switches to address space `root`: writes satp and updates
 * active_root together so the two can never disagree about which
 * space is "current" (get_pte()/the page fault handler both go
 * through active_root, never satp itself -- there's no cheap way to
 * read a plain pointer back out of satp's PPN encoding, so this is
 * the one place that conversion has to happen). sched/riscv64_process.c
 * calls this every time a different process is about to run. */
void paging_activate(unsigned long *root) {
	active_root = root;
	unsigned long satp = SATP_MODE_SV39 | ((unsigned long)root >> 12);
	__builtin_riscv_csrw(CSR_SATP, satp);
	__builtin_riscv_sfence_vma();
}

unsigned long *paging_active_root(void) {
	return active_root;
}

/* Copies the level-1 (2MB) PTE covering `va` from the master kernel
 * table into `dst_root`'s corresponding level-1 slot, creating dst's
 * own level-1 table at that level-2 index first if it doesn't have
 * one yet -- every *other* slot in that level-1 table (and every
 * other level-2 slot entirely) is left alone, untouched, independent.
 *
 * This, not a whole level-2 (1GB) slot, is the actual granularity
 * that needs to be shared -- see paging_new_addrspace()'s comment for
 * why sharing a whole 1GB slot is actively wrong here, not just
 * coarser than necessary. */
static void share_l1_slot(unsigned long *dst_root, unsigned long va) {
	unsigned int l2idx = (va >> 30) & VPN_MASK;
	unsigned long l2ent = root_table[l2idx];
	if (!(l2ent & 1))
		return; /* master doesn't map this range at all */
	unsigned long *master_l1 = (unsigned long *)((l2ent >> PPN_SHIFT) << 12);
	unsigned int l1idx = (va >> 21) & VPN_MASK;
	unsigned long l1ent = master_l1[l1idx];
	if (!(l1ent & 1))
		return;

	unsigned long dst_l2ent = dst_root[l2idx];
	unsigned long *dst_l1;
	if (!(dst_l2ent & 1)) {
		dst_l1 = alloc_table();
		dst_root[l2idx] = (((unsigned long)dst_l1 >> 12) << PPN_SHIFT) | 1;
	} else {
		dst_l1 = (unsigned long *)((dst_l2ent >> PPN_SHIFT) << 12);
	}
	dst_l1[l1idx] = l1ent; /* shares the master's level-0 table for this 2MB window */
}

/* A fresh address space for a new process: its own root table, with
 * just enough of the kernel's own mappings copied in that kernel
 * code/data/the UART stay identically visible from every process --
 * S-mode keeps executing kernel code using whatever satp is currently
 * loaded even while servicing *that process's* trap, so this isn't
 * optional.
 *
 * Deliberately shared at 2MB (level-1 PTE) granularity, not the whole
 * 1GB level-2 slot RAM/UART happen to live in -- checkpoint 6's first
 * real attempt at this shared the whole level-2 slot instead (simpler
 * code, and *sounds* equivalent: "one slot per thing paging_init()
 * maps"), and it was wrong: process_create_from_elf's own stack VA
 * (0xB0000000) and every P4/P5 test's own low ELF-load addresses
 * (0x100b0, 0x800000, 0x900000) all land in the *same* 1GB windows as
 * RAM/UART, just at different 2MB offsets within them. Sharing the
 * whole 1GB slot meant every process's ELF code and stack silently
 * aliased onto the *same* underlying page tables as every other
 * process's -- confirmed the hard way: both of checkpoint 6's two
 * test processes printed the second one's label, because the second
 * process's own ELF-load mapping had overwritten the first's in the
 * page tables both processes were unknowingly sharing. 2MB slices
 * fix it because none of those real per-process addresses happen to
 * fall inside the *specific* 2MB windows RAM/UART actually occupy. */
unsigned long *paging_new_addrspace(void) {
	unsigned long *root = alloc_table();
	for (unsigned long va = RV64_RAM_BASE; va < RV64_MEM_TOP; va += (1UL << 21))
		share_l1_slot(root, va);
	share_l1_slot(root, 0x10000000UL); /* UART0 */
	return root;
}

/* mm/paging_common.c's paging_fork_cow()/paging_ensure_writable()/
 * paging_handle_fault() now do the actual COW copying and fault
 * dispatch, entirely through the accessors above -- this is just the
 * thin trap-decoding wrapper each arch needs (real scause/stval
 * decoding in, arch-specific fatal diagnostics out). */
static void page_fault_handler(struct regs *r) {
	unsigned long fault_addr = r->stval;
	int is_write = (r->scause == 15); /* Store/AMO page fault */
	int from_user = !(r->sstatus & SSTATUS_SPP);

	if (paging_handle_fault(fault_addr, is_write, from_user) == PAGE_FAULT_FIXED)
		return; /* sret retries the faulting instruction, which now succeeds */

	kprintf("\n!! PAGE FAULT at %p (scause=%lu stval=%p %s)\n", (void *)fault_addr,
		r->scause, (void *)fault_addr, is_write ? "write" : "read/exec");
	kprintf("FATAL: unhandled page fault, sepc=%p\n", (void *)r->sepc);
	for (;;) __builtin_riscv_wfi();
}

void paging_init(unsigned long mem_top) {
	for (int i = 0; i < ENTRIES; i++)
		root_table[i] = 0;
	active_root = root_table;

	/* identity map [RV64_RAM_BASE, mem_top) 4KB at a time -- see
	 * mm/pmm.c's phys_base for why RAM doesn't start at 0 here. */
	for (unsigned long addr = 0x80000000UL; addr < mem_top; addr += PAGE_SIZE)
		paging_map_page(addr, addr, PTE_PRESENT | PTE_WRITABLE);

	/* UART0 (arch/risc/riscv64_serial.c) is MMIO, far outside RAM --
	 * unlike i386's port-I/O serial console (a completely separate
	 * address space paging can't affect at all), every kprintf/
	 * serial_putc call on riscv64 is a real memory access that goes
	 * through the page table once this is enabled. Forgetting this
	 * mapping was this port's next bug after arch/risc/riscv64_boot.S's:
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
