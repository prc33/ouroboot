/* Parses an ELF32 i386 executable already sitting in memory (embedded
 * as static data for now -- there's no filesystem yet) and maps every
 * PT_LOAD segment into the current address space.
 *
 * Segments are mapped WRITABLE regardless of p_flags -- plain 32-bit
 * paging (no PAE) has no per-page execute-disable bit anyway, so there
 * is no real W^X to enforce yet, and always-writable is simplest for
 * copying the segment data in. Making .text read-only after load is a
 * real hardening improvement, just not one this loader does yet. */
#include "kernel.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/elf.h"

static unsigned int page_round_down(unsigned int x) {
	return x & ~(PAGE_SIZE - 1);
}
static unsigned int page_round_up(unsigned int x) {
	return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static int load_segment(const unsigned char *data, const Elf32_Phdr *ph) {
	unsigned int seg_start = page_round_down(ph->p_vaddr);
	unsigned int seg_end = page_round_up(ph->p_vaddr + ph->p_memsz);

	for (unsigned int va = seg_start; va < seg_end; va += PAGE_SIZE) {
		unsigned int phys = pmm_alloc_page();
		if (!phys) {
			kprintf("elf: out of memory mapping segment at %p\n", (void *)(unsigned long)va);
			return 0;
		}
		paging_map_page(va, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
		/* zero it first -- covers both real BSS-only pages and the
		 * BSS tail within a partially-file-backed page */
		unsigned char *dst = (unsigned char *)(unsigned long)va;
		for (unsigned int i = 0; i < PAGE_SIZE; i++)
			dst[i] = 0;
	}

	unsigned char *dst = (unsigned char *)(unsigned long)ph->p_vaddr;
	const unsigned char *src = data + ph->p_offset;
	for (unsigned int i = 0; i < ph->p_filesz; i++)
		dst[i] = src[i];

	return 1;
}

unsigned int elf_load(const unsigned char *data, unsigned int size) {
	if (size < sizeof(Elf32_Ehdr)) {
		kprintf("elf: file too small\n");
		return 0;
	}
	const Elf32_Ehdr *eh = (const Elf32_Ehdr *)data;

	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
		kprintf("elf: bad magic\n");
		return 0;
	}
	if (eh->e_ident[4] != 1) { /* ELFCLASS32 */
		kprintf("elf: not 32-bit\n");
		return 0;
	}
	if (eh->e_machine != 3) { /* EM_386 */
		kprintf("elf: not i386 (e_machine=%u)\n", eh->e_machine);
		return 0;
	}
	if (eh->e_type != 2) { /* ET_EXEC -- no PIE/ET_DYN support yet */
		kprintf("elf: not ET_EXEC (e_type=%u) -- PIE not supported yet\n", eh->e_type);
		return 0;
	}

	const Elf32_Phdr *ph = (const Elf32_Phdr *)(data + eh->e_phoff);
	int loaded_any = 0;
	for (int i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;
		if (!load_segment(data, &ph[i]))
			return 0;
		loaded_any = 1;
	}
	if (!loaded_any) {
		kprintf("elf: no PT_LOAD segments\n");
		return 0;
	}

	kprintf("elf: loaded, entry=%p\n", (void *)(unsigned long)eh->e_entry);
	return eh->e_entry;
}
