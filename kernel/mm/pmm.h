#ifndef PMM_H
#define PMM_H

#define PAGE_SIZE 4096u

/* mem_top: absolute address one past the last usable byte of RAM.
 * phys_base: where RAM starts (0 on i386, 0x80000000 on riscv64). */
void pmm_init(unsigned int mem_top, unsigned int phys_base);
unsigned int pmm_alloc_page(void); /* returns physical addr, 0 on failure */
void pmm_free_page(unsigned int addr);
unsigned int pmm_free_pages(void); /* for test assertions */

#endif
