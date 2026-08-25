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

/* Scans from next_free_hint (wrapping around, so this still finds any
 * free page that exists) instead of always restarting at page 0 --
 * real bug in the original always-from-0 version, found running
 * checkpoint 9's much larger, more allocation-heavy kernel image
 * under emulator/web/: every call re-scanned however many already-
 * permanently-used low pages had accumulated so far, an O(total
 * allocations so far) cost *per call* that made the whole boot
 * sequence's cumulative cost effectively quadratic in how much had
 * been allocated -- fine at P1-P8's scale (never enough allocations
 * for the effect to be visible), bad enough by checkpoint 9's real
 * busybox-sized workload that the Wasm emulator's own test genuinely
 * didn't finish in 20 real minutes where QEMU took seconds (QEMU
 * runs the *actual instructions* at native speed regardless of how
 * many extra ones this loop executes; the JS interpreter pays for
 * every one of them). Still worst-case O(total_pages) if the bitmap
 * is nearly full, but the common case (monotonically allocating into
 * still-free space) is now O(1) amortized. */
unsigned int pmm_alloc_page(void) {
	for (unsigned int i = 0; i < total_pages; i++) {
		unsigned int p = (next_free_hint + i) % total_pages;
		if (!bitmap_test(p)) {
			bitmap_set(p);
			free_pages--;
			next_free_hint = p + 1;
			return phys_base + p * PAGE_SIZE;
		}
	}
	return 0; /* out of memory */
}

/* Straight scan from page 0, not next_free_hint -- unlike
 * pmm_alloc_page()'s single-page case, this isn't a hot path (called
 * O(log(file size)) times per file, on growth, not once per byte), so
 * there's no need for its amortized-O(1) trick; starting from 0 finds
 * the lowest-addressed run rather than risking missing one below
 * next_free_hint entirely. */
unsigned int pmm_alloc_contiguous(unsigned int count) {
	if (count == 0 || count > total_pages)
		return 0;
	unsigned int run_start = 0;
	unsigned int run_len = 0;
	for (unsigned int p = 0; p < total_pages; p++) {
		if (!bitmap_test(p)) {
			if (run_len == 0)
				run_start = p;
			run_len++;
			if (run_len == count) {
				for (unsigned int q = run_start; q < run_start + count; q++)
					bitmap_set(q);
				free_pages -= count;
				return phys_base + run_start * PAGE_SIZE;
			}
		} else {
			run_len = 0;
		}
	}
	return 0; /* no run of `count` contiguous free pages -- real fragmentation, not a bug */
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

void pmm_reserve_range(unsigned int lo, unsigned int hi) {
	unsigned int first = (lo - phys_base) / PAGE_SIZE;
	unsigned int last = (hi - phys_base + PAGE_SIZE - 1) / PAGE_SIZE;
	if (last > total_pages)
		last = total_pages;
	for (unsigned int p = first; p < last; p++) {
		if (!bitmap_test(p)) {
			bitmap_set(p);
			free_pages--;
		}
	}
}
