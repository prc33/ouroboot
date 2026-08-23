#ifndef ELF_H
#define ELF_H

typedef struct {
	unsigned char e_ident[16];
	unsigned short e_type;
	unsigned short e_machine;
	unsigned int e_version;
	unsigned int e_entry;
	unsigned int e_phoff;
	unsigned int e_shoff;
	unsigned int e_flags;
	unsigned short e_ehsize;
	unsigned short e_phentsize;
	unsigned short e_phnum;
	unsigned short e_shentsize;
	unsigned short e_shnum;
	unsigned short e_shstrndx;
} Elf32_Ehdr;

typedef struct {
	unsigned int p_type;
	unsigned int p_offset;
	unsigned int p_vaddr;
	unsigned int p_paddr;
	unsigned int p_filesz;
	unsigned int p_memsz;
	unsigned int p_flags;
	unsigned int p_align;
} Elf32_Phdr;

#define PT_LOAD 1

#define ELFMAG "\x7f""ELF"

/* Loads every PT_LOAD segment from `data` (a complete ELF file already
 * in memory) into the current address space. Returns the ELF entry
 * point, or 0 on any validation failure. */
unsigned int elf_load(const unsigned char *data, unsigned int size);

#endif
