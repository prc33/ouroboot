/* Bitmap physical page allocator. Covers up to MAX_MEMORY_MB of RAM --
 * fixed-size static bitmap rather than a dynamically-sized one, since
 * we don't have a working allocator yet to size one with (chicken and
 * egg); 128MB is comfortably more than any phase up to and including
 * the kernel-builds-itself milestone needs. */
#include "kernel.h"
#include "pmm.h"

#define MAX_MEMORY_MB 128u
#define MAX_PAGES (MAX_MEMORY_MB * 1024u * 1024u / PAGE_SIZE)
#define BITMAP_WORDS (MAX_PAGES / 32)

/* Provided by arch/kend.S -- see that file for why it must be the last
 * object on the link line to be accurate. Only its ADDRESS matters. */
extern unsigned char kernel_end;

static unsigned int bitmap[BITMAP_WORDS];
static unsigned int total_pages;
static unsigned int free_pages;

static inline void bitmap_set(unsigned int page) {
	bitmap[page / 32] |= (1u << (page % 32));
}
static inline void bitmap_clear(unsigned int page) {
	bitmap[page / 32] &= ~(1u << (page % 32));
}
static inline int bitmap_test(unsigned int page) {
	return (bitmap[page / 32] >> (page % 32)) & 1;
}

void pmm_init(unsigned int mem_upper_kb) {
	unsigned int top_addr = 0x100000u + mem_upper_kb * 1024u;
	total_pages = top_addr / PAGE_SIZE;
	if (total_pages > MAX_PAGES)
		total_pages = MAX_PAGES;

	/* start all pages (including beyond total_pages, up to MAX_PAGES)
	 * marked USED, then free exactly the usable range -- this way
	 * "used" is the safe default rather than something we have to
	 * remember to apply everywhere */
	for (unsigned int i = 0; i < BITMAP_WORDS; i++)
		bitmap[i] = 0xFFFFFFFFu;

	/* everything from 1MB (where our kernel is loaded) up through the
	 * page containing kernel_end is not free -- it's us. Everything
	 * below 1MB (real-mode IVT, BIOS data area, video memory) we
	 * simply never hand out either; not worth mapping/using yet. */
	unsigned int kend = (unsigned int)(unsigned long)&kernel_end;
	unsigned int first_free_page = (kend + PAGE_SIZE - 1) / PAGE_SIZE;

	free_pages = 0;
	for (unsigned int p = first_free_page; p < total_pages; p++) {
		bitmap_clear(p);
		free_pages++;
	}

	kprintf("pmm: %u KB usable, kernel ends at %p, %u pages free (%u KB)\n",
		mem_upper_kb, (void *)(unsigned long)kend, free_pages, free_pages * 4);
}

unsigned int pmm_alloc_page(void) {
	for (unsigned int p = 0; p < total_pages; p++) {
		if (!bitmap_test(p)) {
			bitmap_set(p);
			free_pages--;
			return p * PAGE_SIZE;
		}
	}
	return 0; /* out of memory */
}

void pmm_free_page(unsigned int addr) {
	unsigned int p = addr / PAGE_SIZE;
	if (p < total_pages && bitmap_test(p)) {
		bitmap_clear(p);
		free_pages++;
	}
}

unsigned int pmm_free_pages(void) {
	return free_pages;
}
