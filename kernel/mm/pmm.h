#ifndef PMM_H
#define PMM_H

#define PAGE_SIZE 4096u

void pmm_init(unsigned int mem_upper_kb);
unsigned int pmm_alloc_page(void); /* returns physical addr, 0 on failure */
void pmm_free_page(unsigned int addr);
unsigned int pmm_free_pages(void); /* for test assertions */

#endif
