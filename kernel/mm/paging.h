#ifndef PAGING_H
#define PAGING_H

#ifndef KERNEL_ARCH_RISCV64
#define PTE_PRESENT  0x001u
#define PTE_WRITABLE 0x002u
#define PTE_USER     0x004u
#define PTE_COW      0x200u /* bit 9: OS-available, marks a copy-on-write page */

void paging_init(unsigned int mem_upper_kb);
void paging_map_page(unsigned int virt, unsigned int phys, unsigned int flags);
unsigned int paging_get_phys(unsigned int virt); /* 0 if unmapped */
#else
/* Sv39 PTE bits differ entirely from i386's (V/R/W/X/U vs a single
 * flat present/writable/user, and RISC-V *requires* the X bit for a
 * page to be fetched as code at all -- no implicit "everything's
 * executable" the way non-PAE i386 has). Same symbolic names, same
 * "no real W^X yet" simplification as i386 (see mm/riscv64_paging.c
 * and mm/elf.c): PTE_PRESENT alone already grants read+execute, so
 * every caller's existing `PTE_PRESENT | PTE_WRITABLE | PTE_USER`
 * expression means the same thing conceptually on both targets. */
#define PTE_PRESENT  0x00bu /* V | R | X */
#define PTE_WRITABLE 0x004u /* W */
#define PTE_USER     0x010u /* U */
#define PTE_COW      0x100u /* bit 8: RSW[0], OS-available, marks a copy-on-write page */

void paging_init(unsigned long mem_top);
void paging_map_page(unsigned long virt, unsigned long phys, unsigned long flags);
unsigned long paging_get_phys(unsigned long virt); /* 0 if unmapped */
#endif

#endif
