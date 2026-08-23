/* Bitmap physical page allocator. Covers up to MAX_MEMORY_MB of RAM --
 * fixed-size static bitmap rather than a dynamically-sized one, since
 * we don't have a working allocator yet to size one with (chicken and
 * egg); 128MB is comfortably more than any phase up to and including
 * the kernel-builds-itself milestone needs.
 *
 * phys_base: i386 RAM starts at physical 0, so it's always 0 there.
 * riscv64 RAM starts at 0x80000000 (QEMU virt machine) -- without
 * this, the bitmap would need to cover pages 0..0x80000000/PAGE_SIZE
 * just to reach the start of usable memory, wasting almost all of it.
 * Internally the bitmap is indexed by page number *relative to*
 * phys_base; every address crossing this file's API boundary
 * (pmm_alloc_page's return value, pmm_free_page's argument) is an
 * absolute physical address, same as before this parameter existed. */
#include "kernel.h"
#include "pmm.h"

#define MAX_MEMORY_MB 128u
#define MAX_PAGES (MAX_MEMORY_MB * 1024u * 1024u / PAGE_SIZE)
#define BITMAP_WORDS (MAX_PAGES / 32)

/* Provided by arch/kend.S (i386) or arch/riscv64_kend.S (riscv64) --
 * see those files for why they must be the last object on the link
 * line to be accurate. Only its ADDRESS matters. */
extern unsigned char kernel_end;

static unsigned int bitmap[BITMAP_WORDS];
static unsigned int total_pages;
static unsigned int free_pages;
static unsigned int phys_base;

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

	/* everything from the kernel's load address up through the page
	 * containing kernel_end is not free -- it's us. Everything below
	 * that (on i386: real-mode IVT, BIOS data area, video memory) we
	 * simply never hand out either; not worth mapping/using yet. */
	unsigned int kend = (unsigned int)(unsigned long)&kernel_end;
	unsigned int first_free_page = (kend - phys_base + PAGE_SIZE - 1) / PAGE_SIZE;

	free_pages = 0;
	for (unsigned int p = first_free_page; p < total_pages; p++) {
		bitmap_clear(p);
		free_pages++;
	}

	kprintf("pmm: kernel ends at %p, %u pages free (%u KB)\n",
		(void *)(unsigned long)kend, free_pages, free_pages * 4);
}

unsigned int pmm_alloc_page(void) {
	for (unsigned int p = 0; p < total_pages; p++) {
		if (!bitmap_test(p)) {
			bitmap_set(p);
			free_pages--;
			return phys_base + p * PAGE_SIZE;
		}
	}
	return 0; /* out of memory */
}

void pmm_free_page(unsigned int addr) {
	unsigned int p = (addr - phys_base) / PAGE_SIZE;
	if (p < total_pages && bitmap_test(p)) {
		bitmap_clear(p);
		free_pages++;
	}
}

unsigned int pmm_free_pages(void) {
	return free_pages;
}
