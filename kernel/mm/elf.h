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

/* ELFCLASS64 shapes -- field order differs from ELF32, not just
 * widths (e_flags moves after e_entry/e_phoff/e_shoff, which are
 * themselves 8 bytes instead of 4; Phdr reorders p_flags right after
 * p_type, ahead of the offset/addr fields, since p_flags is no longer
 * the odd one out padding-wise on a 64-bit target). See the ELF64
 * spec, not a project-specific choice. */
typedef struct {
	unsigned char e_ident[16];
	unsigned short e_type;
	unsigned short e_machine;
	unsigned int e_version;
	unsigned long e_entry;
	unsigned long e_phoff;
	unsigned long e_shoff;
	unsigned int e_flags;
	unsigned short e_ehsize;
	unsigned short e_phentsize;
	unsigned short e_phnum;
	unsigned short e_shentsize;
	unsigned short e_shnum;
	unsigned short e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	unsigned int p_type;
	unsigned int p_flags;
	unsigned long p_offset;
	unsigned long p_vaddr;
	unsigned long p_paddr;
	unsigned long p_filesz;
	unsigned long p_memsz;
	unsigned long p_align;
} Elf64_Phdr;

#define PT_LOAD 1

#define ELFMAG "\x7f""ELF"

/* Loads every PT_LOAD segment from `data` (a complete ELF file already
 * in memory) into the current address space. Returns the ELF entry
 * point, or 0 on any validation failure. Handles both ELFCLASS32
 * (i386, EM_386) and ELFCLASS64 (riscv64, EM_RISCV), branching on
 * e_ident[EI_CLASS] at the top of elf_load() -- one shared loader,
 * i386's own path byte-for-byte unchanged. */
unsigned long elf_load(const unsigned char *data, unsigned long size);

#endif
