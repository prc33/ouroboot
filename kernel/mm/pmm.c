/* Bitmap allocator. Bitmap indices are relative to each architecture's RAM
 * base; public addresses are absolute. */
#include "kernel.h"
#include "pmm.h"

#define MAX_MEMORY_MB 128u
#define MAX_PAGES (MAX_MEMORY_MB * 1024u * 1024u / PAGE_SIZE)
#define BITMAP_WORDS (MAX_PAGES / 32)

/* Provided by arch/i386/kend.S (i386) or arch/risc/riscv64_kend.S (riscv64) --
 * see those files for why they must be the last object on the link
 * line to be accurate. Only its ADDRESS matters. */
extern unsigned char kernel_end;

static unsigned int bitmap[BITMAP_WORDS];
static unsigned short refs[MAX_PAGES];
static unsigned int total_pages;
static unsigned int free_pages;
static unsigned int phys_base;
static unsigned int next_free_hint; /* see pmm_alloc_page()'s own comment */

static inline void bitmap_set(unsigned int page) {
	bitmap[page / 32] |= (1u << (page % 32));
}
static inline void bitmap_clear(unsigned int page) {
	bitmap[page / 32] &= ~(1u << (page % 32));
}
static inline int bitmap_test(unsigned int page) {
	return (bitmap[page / 32] >> (page % 32)) & 1;
}

void pmm_init(unsigned int mem_top, unsigned int base) {
	phys_base = base;
	total_pages = (mem_top - phys_base) / PAGE_SIZE;
	if (total_pages > MAX_PAGES)
		total_pages = MAX_PAGES;

	/* start all pages (including beyond total_pages, up to MAX_PAGES)
	 * marked USED, then free exactly the usable range -- this way
	 * "used" is the safe default rather than something we have to
	 * remember to apply everywhere */
	for (unsigned int i = 0; i < BITMAP_WORDS; i++)
		bitmap[i] = 0xFFFFFFFFu;
	for (unsigned int i = 0; i < MAX_PAGES; i++)
		refs[i] = 1;

	/* everything from the kernel's load address up through the page
	 * containing kernel_end is not free -- it's us. Everything below
	 * that (on i386: real-mode IVT, BIOS data area, video memory) we
	 * simply never hand out either; not worth mapping/using yet. */
	unsigned int kend = (unsigned int)(unsigned long)&kernel_end;
	unsigned int first_free_page = (kend - phys_base + PAGE_SIZE - 1) / PAGE_SIZE;

	free_pages = 0;
	for (unsigned int p = first_free_page; p < total_pages; p++) {
		bitmap_clear(p);
		refs[p] = 0;
		free_pages++;
	}

	kprintf("pmm: kernel ends at %p, %u pages free (%u KB)\n",
		(void *)(unsigned long)kend, free_pages, free_pages * 4);
}

/* The rotating hint makes sequential allocation amortized O(1). */
unsigned int pmm_alloc_page(void) {
	for (unsigned int i = 0; i < total_pages; i++) {
		unsigned int p = (next_free_hint + i) % total_pages;
		if (!bitmap_test(p)) {
			bitmap_set(p);
			refs[p] = 1;
			free_pages--;
			next_free_hint = p + 1;
			return phys_base + p * PAGE_SIZE;
		}
	}
	return 0; /* out of memory */
}

/* Contiguous allocation scans from zero so it cannot miss an earlier run. */
unsigned int pmm_alloc_contiguous(unsigned int count) {
	if (count == 0 || count > total_pages)
		return 0;
	if (count == 1)
		return pmm_alloc_page();
	unsigned int run_start = 0;
	unsigned int run_len = 0;
	for (unsigned int p = 0; p < total_pages; p++) {
		if (!bitmap_test(p)) {
			if (run_len == 0)
				run_start = p;
			run_len++;
			if (run_len == count) {
				for (unsigned int q = run_start; q < run_start + count; q++)
					bitmap_set(q), refs[q] = 1;
				free_pages -= count;
				return phys_base + run_start * PAGE_SIZE;
			}
		} else {
			run_len = 0;
		}
	}
	return 0; /* no run of `count` contiguous free pages -- real fragmentation, not a bug */
}

void pmm_retain_page(unsigned int addr) {
	unsigned int p = (addr - phys_base) / PAGE_SIZE;
	if (p < total_pages && bitmap_test(p) && refs[p] != 0xffff)
		refs[p]++;
}

void pmm_free_page(unsigned int addr) {
	unsigned int p = (addr - phys_base) / PAGE_SIZE;
	if (p < total_pages && bitmap_test(p) && refs[p] && --refs[p] == 0) {
		bitmap_clear(p);
		free_pages++;
	}
}

unsigned int pmm_free_pages(void) {
	return free_pages;
}

void pmm_reserve_range(unsigned int lo, unsigned int hi) {
	unsigned int first = (lo - phys_base) / PAGE_SIZE;
	unsigned int last = (hi - phys_base + PAGE_SIZE - 1) / PAGE_SIZE;
	if (last > total_pages)
		last = total_pages;
	for (unsigned int p = first; p < last; p++) {
		if (!bitmap_test(p)) {
			bitmap_set(p);
			refs[p] = 1;
			free_pages--;
		}
	}
}
