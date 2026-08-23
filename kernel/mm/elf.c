/* Parses an ELF executable already sitting in memory (embedded as
 * static data for now -- there's no filesystem yet) and maps every
 * PT_LOAD segment into the current address space. Handles both
 * ELFCLASS32 (i386, EM_386) and ELFCLASS64 (riscv64, EM_RISCV) --
 * branches on e_ident[EI_CLASS] right at the top of elf_load(); i386's
 * own path is otherwise byte-for-byte what it always was.
 *
 * Segments are mapped WRITABLE regardless of p_flags -- i386 has no
 * real W^X to enforce yet either way (no PAE, no per-page execute-
 * disable bit); riscv64 *does* have a real X bit (Sv39 PTEs), but
 * PTE_PRESENT already includes it unconditionally (see mm/paging.h)
 * for exactly this same "no real W^X yet" reason. Making .text
 * read-only/PTE_PRESENT-without-PTE_WRITABLE after load is a real
 * hardening improvement neither loader does yet. */
#include "kernel.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/elf.h"

static unsigned long page_round_down(unsigned long x) {
	return x & ~(PAGE_SIZE - 1UL);
}
static unsigned long page_round_up(unsigned long x) {
	return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1UL);
}

static int load_segment(const unsigned char *data, unsigned long p_offset,
                         unsigned long p_vaddr, unsigned long p_filesz, unsigned long p_memsz) {
	unsigned long seg_start = page_round_down(p_vaddr);
	unsigned long seg_end = page_round_up(p_vaddr + p_memsz);

	for (unsigned long va = seg_start; va < seg_end; va += PAGE_SIZE) {
		unsigned long phys = pmm_alloc_page();
		if (!phys) {
			kprintf("elf: out of memory mapping segment at %p\n", (void *)va);
			return 0;
		}
		paging_map_page(va, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
		/* zero it first -- covers both real BSS-only pages and the
		 * BSS tail within a partially-file-backed page */
		unsigned char *dst = (unsigned char *)va;
		for (unsigned int i = 0; i < PAGE_SIZE; i++)
			dst[i] = 0;
	}

	unsigned char *dst = (unsigned char *)p_vaddr;
	const unsigned char *src = data + p_offset;
	for (unsigned long i = 0; i < p_filesz; i++)
		dst[i] = src[i];

	return 1;
}

static unsigned long elf_load32(const unsigned char *data, unsigned long size) {
	if (size < sizeof(Elf32_Ehdr)) {
		kprintf("elf: file too small\n");
		return 0;
	}
	const Elf32_Ehdr *eh = (const Elf32_Ehdr *)data;

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
		if (!load_segment(data, ph[i].p_offset, ph[i].p_vaddr, ph[i].p_filesz, ph[i].p_memsz))
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

static unsigned long elf_load64(const unsigned char *data, unsigned long size) {
	if (size < sizeof(Elf64_Ehdr)) {
		kprintf("elf: file too small\n");
		return 0;
	}
	const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;

	if (eh->e_machine != 243) { /* EM_RISCV */
		kprintf("elf: not riscv64 (e_machine=%u)\n", eh->e_machine);
		return 0;
	}
	if (eh->e_type != 2) { /* ET_EXEC -- no PIE/ET_DYN support yet */
		kprintf("elf: not ET_EXEC (e_type=%u) -- PIE not supported yet\n", eh->e_type);
		return 0;
	}

	const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + eh->e_phoff);
	int loaded_any = 0;
	for (int i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;
		if (!load_segment(data, ph[i].p_offset, ph[i].p_vaddr, ph[i].p_filesz, ph[i].p_memsz))
			return 0;
		loaded_any = 1;
	}
	if (!loaded_any) {
		kprintf("elf: no PT_LOAD segments\n");
		return 0;
	}

	kprintf("elf: loaded, entry=%p\n", (void *)eh->e_entry);
	return eh->e_entry;
}

unsigned long elf_load(const unsigned char *data, unsigned long size) {
	if (size < 16) {
		kprintf("elf: file too small\n");
		return 0;
	}
	if (data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') {
		kprintf("elf: bad magic\n");
		return 0;
	}
	if (data[4] == 1) /* EI_CLASS: ELFCLASS32 */
		return elf_load32(data, size);
	if (data[4] == 2) /* ELFCLASS64 */
		return elf_load64(data, size);
	kprintf("elf: unknown class %u\n", data[4]);
	return 0;
}
