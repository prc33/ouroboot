#include "kernel.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "syscall_common.h"

#define EINVAL  22
#define ENOTTY  25
#define TIOCGWINSZ 0x5413

unsigned long syscall_page_round_up(unsigned long x) {
	return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1UL);
}

void syscall_write_raw(unsigned long fd, const char *buf, unsigned long count) {
	if (fd == 1 || fd == 2) {
		for (unsigned long i = 0; i < count; i++)
			serial_putc(buf[i]);
	}
}

long syscall_ioctl_check(unsigned long fd, unsigned long req) {
	if ((fd == 1 || fd == 2) && req == TIOCGWINSZ)
		return -ENOTTY;
	return -EINVAL;
}

int syscall_grow_pages(unsigned long old_top, unsigned long new_top) {
	for (unsigned long va = old_top; va < new_top; va += PAGE_SIZE) {
		unsigned long phys = pmm_alloc_page();
		if (!phys)
			return 0;
		/* pmm_alloc_page() does not clear memory -- see this
		 * function's own header comment for why that matters here. */
		unsigned long *words = (unsigned long *)phys;
		for (unsigned int i = 0; i < PAGE_SIZE / sizeof(unsigned long); i++)
			words[i] = 0;
		paging_map_page(va, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
	}
	return 1;
}

void syscall_munmap_pages(unsigned long addr, unsigned long len) {
	for (unsigned long va = addr; va < addr + len; va += PAGE_SIZE) {
		unsigned long phys = paging_get_phys(va);
		if (phys) {
			paging_map_page(va, 0, 0); /* clear PTE_PRESENT */
			pmm_free_page(phys & ~0xFFFUL);
		}
	}
}
