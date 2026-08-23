#ifndef PAGING_H
#define PAGING_H

#define PTE_PRESENT  0x001u
#define PTE_WRITABLE 0x002u
#define PTE_USER     0x004u
#define PTE_COW      0x200u /* bit 9: OS-available, marks a copy-on-write page */

void paging_init(unsigned int mem_upper_kb);
void paging_map_page(unsigned int virt, unsigned int phys, unsigned int flags);
unsigned int paging_get_phys(unsigned int virt); /* 0 if unmapped */

#endif
