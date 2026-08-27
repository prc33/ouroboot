/* Page-fault / copy-on-write logic that turned out to be identical
 * between i386 and riscv64 once actually written down side by side --
 * checkpoint 17 (docs/kernel-arch-split-plan.md's "genericize rather
 * than write afresh" instruction, applied to the i386 paging port).
 * None of what's here needs to know how many levels a page-table walk
 * takes, or what the leaf PTE's bit layout is -- it only ever touches
 * an address space through the small paging_*_in()/paging_map_page()/
 * paging_get_phys()/paging_get_flags() accessor set each arch's own
 * paging.c (arch/i386/, arch/risc/) implements, the same way mm/elf.c
 * and sched/process.c already do.
 *
 * What's genuinely NOT here, and stays arch-specific by necessity: the
 * walk itself (2-level i386 vs 3-level Sv39), page-table allocation,
 * paging_new_addrspace()'s kernel-sharing granularity (differs: 4MB
 * i386 page-directory-entry vs 2MB riscv64 level-1 slot -- see each
 * arch's own paging_new_addrspace() for why), CR3/satp register
 * access, and the TLB-flush instruction (paging_flush_tlb(), one line
 * per arch).
 */
#include "kernel.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "sched/process.h"

/* The actual copy-and-remap: shared by paging_handle_fault() (the
 * hardware path, a real user-mode write fault) and
 * paging_ensure_writable() (the software path -- see its own comment
 * below). Returns 1 on success, 0 on OOM. */
static int fix_cow_page(unsigned long page) {
	unsigned long old_phys = paging_get_phys(page) & ~0xFFFUL;
	unsigned long flags = paging_get_flags(page);
	unsigned long new_phys = pmm_alloc_page();
	if (!new_phys)
		return 0;
	unsigned char *src = (unsigned char *)old_phys;
	unsigned char *dst = (unsigned char *)new_phys;
	for (unsigned int i = 0; i < PAGE_SIZE; i++)
		dst[i] = src[i];

	/* Preserve PTE_USER from the pre-copy flags rather than hardcoding
	 * just PRESENT|WRITABLE -- real bug this is a regression test for
	 * (checkpoint 7's fork() test, back when this lived only in
	 * arch/risc/riscv64_paging.c, see that file's git history):
	 * dropping PTE_USER here made a retried U-mode access fault
	 * *again*, for a different reason the caller's COW check doesn't
	 * recognize. */
	paging_map_page(page, new_phys, PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER));
	pmm_free_page((unsigned int)old_phys);
	paging_flush_tlb();
	return 1;
}

int paging_handle_fault(unsigned long fault_addr, int is_write, int from_user) {
	unsigned long page = fault_addr & ~0xFFFUL;
	unsigned long flags = paging_get_flags(page);

	if (is_write && (flags & PTE_PRESENT) && (flags & PTE_COW))
		return fix_cow_page(page) ? PAGE_FAULT_FIXED : PAGE_FAULT_FATAL;

	/* Linux-style lazy user stack: exec reserves an address range but
	 * maps only its top pages. A user fault below the committed
	 * portion allocates one zero page; the faulting instruction is
	 * then retried. Never applies to a fault the kernel itself takes
	 * (from_user == 0) -- same restriction the riscv64-only version of
	 * this check always had (SSTATUS_SPP clear). */
	if (from_user && !(flags & PTE_PRESENT) && process_handle_stack_fault(fault_addr))
		return PAGE_FAULT_FIXED;

	return PAGE_FAULT_FATAL;
}

/* checkpoint 10 (originally riscv64-only -- see arch/risc/riscv64_paging.c's
 * git history for the real bug that motivated it): resolves any COW
 * page in [addr, addr+len) against the *active* address space,
 * entirely in C, no trap involved -- call before any kernel write into
 * a caller-supplied (user) buffer that might still be COW-marked.
 * Real bug this avoids: a page fault taken *while already servicing a
 * syscall trap* is a nested trap neither arch's single-trapframe-per-
 * process design can survive cleanly mid-handler (arch/risc/riscv64_trap_entry.S's
 * own comment) -- i386's isr_common_stub has the same "one trap at a
 * time" shape, so this applies unchanged there too, which is exactly
 * why it's over here now instead of staying riscv64-only. */
void paging_ensure_writable(unsigned long addr, unsigned long len) {
	if (len == 0)
		return;
	unsigned long start = addr & ~0xFFFUL;
	unsigned long end = (addr + len - 1) & ~0xFFFUL;
	for (unsigned long page = start; page <= end; page += PAGE_SIZE) {
		unsigned long flags = paging_get_flags(page);
		if ((flags & PTE_PRESENT) && (flags & PTE_COW))
			fix_cow_page(page); /* OOM here just leaves it COW -- the
			                      * caller's own write then faults for
			                      * real and hits the ordinary OOM path
			                      * there (not nested -- this isn't a
			                      * trap). */
	}
}

/* Clones the user-space mappings in [lo, hi) from src into dst,
 * marking both copies PTE_COW (shared physical page, not writable)
 * instead of actually duplicating the data -- exactly
 * paging_handle_fault()'s COW mechanism above, just applied to a
 * whole process's address space instead of one faulting page. Skips
 * pages already unmapped in src; a page already COW in src is
 * re-marked (a no-op flag-wise) and shared again, which is correct
 * for a fork of a process that's itself a fork-child. */
void paging_fork_cow(unsigned long *dst_root, unsigned long *src_root, unsigned long lo, unsigned long hi) {
	for (unsigned long va = lo; va < hi; va += PAGE_SIZE) {
		unsigned long phys = paging_get_phys_in(src_root, va);
		if (!phys)
			continue;
		unsigned long flags = paging_get_flags_in(src_root, va);
		if (!(flags & PTE_PRESENT))
			continue;
		unsigned long new_flags = (flags & ~(unsigned long)PTE_WRITABLE) | PTE_COW;
		phys &= ~0xFFFUL;
		pmm_retain_page((unsigned int)phys);
		paging_map_page_in(src_root, va, phys, new_flags);
		paging_map_page_in(dst_root, va, phys, new_flags);
	}
	if (src_root == paging_active_root() || dst_root == paging_active_root())
		paging_flush_tlb();
}
