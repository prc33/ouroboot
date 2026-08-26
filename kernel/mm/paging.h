#ifndef PAGING_H
#define PAGING_H

/* PTE bit VALUES are the one thing that's genuinely different bit-for-
 * bit between the two page-table formats (i386's flat present/
 * writable/user vs Sv39's V/R/W/X/U, and RISC-V *requires* the X bit
 * for a page to be fetched as code at all -- no implicit "everything's
 * executable" the way non-PAE i386 has). Same symbolic names either
 * way, same "no real W^X yet" simplification on both targets:
 * PTE_PRESENT alone already grants read+execute, so every caller's
 * `PTE_PRESENT | PTE_WRITABLE | PTE_USER` expression means the same
 * thing conceptually on both. */
#ifndef KERNEL_ARCH_RISCV64
#define PTE_PRESENT  0x001u
#define PTE_WRITABLE 0x002u
#define PTE_USER     0x004u
#define PTE_COW      0x200u /* bit 9: OS-available, marks a copy-on-write page */
#else
#define PTE_PRESENT  0x00bu /* V | R | X */
#define PTE_WRITABLE 0x004u /* W */
#define PTE_USER     0x010u /* U */
#define PTE_COW      0x100u /* bit 8: RSW[0], OS-available, marks a copy-on-write page */
#endif

/* Everything below is one shared interface both arch/i386/paging.c and
 * arch/risc/riscv64_paging.c implement in full -- checkpoint 17
 * (docs/kernel-arch-split-plan.md): i386's own version used to only
 * cover the original 3 single-address-space functions, riscv64's own
 * per-process additions (paging_get_flags, paging_new_addrspace,
 * paging_activate, ...) living behind an #ifdef only it satisfied.
 * Widening i386's implementation to the same shape (mm/paging_common.c's
 * own file comment has the details of what that unlocked) means one
 * signature set now genuinely describes both. `unsigned long` throughout
 * even for i386 (where every real value fits in 32 bits) rather than
 * `unsigned int` -- TCC narrows a wider argument down to a 32-bit
 * parameter silently, so this is only ever a widening for i386 callers,
 * never a behavior change, and it's what let this become one call
 * surface instead of two near-identical ones. */

void paging_init(unsigned long mem_top); /* i386: KB above 1MB from Multiboot; riscv64: physical top address */
void paging_map_page(unsigned long virt, unsigned long phys, unsigned long flags);
unsigned long paging_get_phys(unsigned long virt); /* 0 if unmapped */

/* The leaf PTE's flag bits covering `virt` (active address space), or 0
 * if unmapped -- riscv64_kmain.c's run_cow_user_test() (test/riscv64_checkpoints.c)
 * uses this to directly confirm PTE_USER survives a COW copy. */
unsigned long paging_get_flags(unsigned long virt);

/* Per-address-space variants, for sched/process.c. */
unsigned long *paging_new_addrspace(void);
void paging_activate(unsigned long *root);
unsigned long *paging_active_root(void);
void paging_map_page_in(unsigned long *root, unsigned long virt, unsigned long phys, unsigned long flags);
unsigned long paging_get_phys_in(unsigned long *root, unsigned long virt);
unsigned long paging_get_flags_in(unsigned long *root, unsigned long virt);

/* One TLB-invalidation instruction, whatever that is on this arch
 * (sfence.vma vs a full CR3 reload) -- mm/paging_common.c's own
 * paging_fork_cow() is the only generic caller; each arch's own
 * paging_map_page_in()/paging_init() still do their own inline
 * invalidation (invlpg / sfence.vma) for the single-page case, which
 * this doesn't replace. */
void paging_flush_tlb(void);

/* mm/paging_common.c -- genuinely identical between i386 and riscv64
 * once written down side by side, see that file's own comment. */
void paging_fork_cow(unsigned long *dst_root, unsigned long *src_root, unsigned long lo, unsigned long hi);
void paging_ensure_writable(unsigned long addr, unsigned long len);

#define PAGE_FAULT_FIXED 0 /* resolved (COW-copied, or grew the stack) -- caller should retry the faulting instruction */
#define PAGE_FAULT_FATAL 1 /* unhandled -- caller should print its own diagnostics and halt */

/* The arch-neutral half of page-fault handling: given a fault already
 * decoded into (address, was-it-a-write, was-the-CPU-in-user-mode) by
 * each arch's own trap handler, decides whether it's a COW copy, a
 * lazy user-stack growth, or genuinely fatal. See mm/paging_common.c's
 * own comment for why this doesn't need to know anything about page
 * table layout to do that. */
int paging_handle_fault(unsigned long fault_addr, int is_write, int from_user);

#endif
