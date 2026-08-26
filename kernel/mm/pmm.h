#ifndef PMM_H
#define PMM_H

#define PAGE_SIZE 4096u

/* mem_top: absolute address one past the last usable byte of RAM.
 * phys_base: where RAM starts (0 on i386, 0x80000000 on riscv64). */
void pmm_init(unsigned int mem_top, unsigned int phys_base);
unsigned int pmm_alloc_page(void); /* returns physical addr, 0 on failure */
void pmm_free_page(unsigned int addr);
unsigned int pmm_free_pages(void); /* for test assertions */

/* Like pmm_alloc_page(), but `count` pages guaranteed *contiguous* --
 * for a caller that wants to treat the result as one plain array
 * (mm/ramfs.c's own writable-file backing store, which needs simple
 * byte-offset indexing, not a page-table-style indirect lookup).
 * Returns 0 (and reserves nothing) if no run of `count` free pages
 * exists, same failure contract as pmm_alloc_page() -- an ordinary,
 * expected outcome under real fragmentation, not a fatal one; callers
 * are expected to propagate it as ENOMEM, not crash. */
unsigned int pmm_alloc_contiguous(unsigned int count);

/* Marks every page in [lo, hi) used, without anyone having to
 * pmm_alloc_page() them first -- for memory this allocator doesn't
 * itself know is spoken for. riscv64_kmain.c uses this right after
 * pmm_init() to reserve arch/risc/riscv64_memmap.h's hardcoded scratch
 * region (boot stack, trap dispatch pointer, trapframe, trap stack):
 * pmm_init() only ever reserved [phys_base, kernel_end) -- the
 * scratch region lives *above* kernel_end (deliberately, so it survives
 * kernel image growth -- see riscv64_memmap.h), so without this,
 * pmm_alloc_page() eventually hands it out like any other free page.
 * Found the hard way: harmless for every checkpoint through P5 (never
 * enough allocations happened to reach that far up), until checkpoint
 * 6/7's real ELF loads finally did -- one of them got handed the page
 * *underneath the kernel's own currently-running boot stack*, and
 * zeroing it (mm/elf.c's own BSS-zeroing loop) corrupted the C call
 * chain actually in progress, silently, with no fault or error
 * message at all (nothing had *mis-executed* yet, just had its own
 * memory overwritten out from under it). */
void pmm_reserve_range(unsigned int lo, unsigned int hi);

#endif
