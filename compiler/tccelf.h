#ifndef TCC_ELF_H
#define TCC_ELF_H

#include "common.h"
#include "target.h"
#include "elf.h"
#include "utils.h"

#if PTR_SIZE == 8
# define ELFCLASSW ELFCLASS64
# define ElfW(type) Elf##64##_##type
# define ELFW(type) ELF##64_##type
# define ElfW_Rel ElfW(Rela)
# define SHT_RELX SHT_RELA
# define REL_SECTION_FMT ".rela%s"
#else
# define ELFCLASSW ELFCLASS32
# define ElfW(type) Elf##32##_##type
# define ELFW(type) ELF##32_##type
# define ElfW_Rel ElfW(Rel)
# define SHT_RELX SHT_REL
# define REL_SECTION_FMT ".rel%s"
#endif

#define addr_t ElfW(Addr)
#define ElfSym ElfW(Sym)

typedef struct TCCState TCCState;
typedef struct Sym Sym;

typedef struct Section {
    unsigned long data_offset;
    unsigned char *data;
    unsigned long data_allocated;
    TCCState *s1;
    int sh_name, sh_num, sh_type, sh_flags, sh_info;
    int sh_addralign, sh_entsize;
    unsigned long sh_size;
    addr_t sh_addr;
    unsigned long sh_offset;
    int nb_hashed_syms;
    struct Section *link, *reloc, *hash, *prev;
    char name[1];
} Section;

#define ARMAG "!<arch>\012"
#define ARFMAG "`\n"

typedef struct ArchiveHeader {
    char ar_name[16];
    char ar_date[12];
    char ar_uid[6];
    char ar_gid[6];
    char ar_mode[8];
    char ar_size[10];
    char ar_fmag[2];
} ArchiveHeader;

ST_FUNC int tcc_tool_ar(int argc, char **argv);

ST_FUNC void tccelf_new(TCCState *s);
ST_FUNC void tccelf_delete(TCCState *s);
ST_FUNC void tccelf_begin_file(TCCState *s);
ST_FUNC void tccelf_end_file(TCCState *s);
ST_FUNC Section *new_section(TCCState *s, const char *name, int type, int flags);
ST_FUNC void section_realloc(Section *sec, unsigned long size);
ST_FUNC size_t section_add(Section *sec, addr_t size, int align);
ST_FUNC void *section_ptr_add(Section *sec, addr_t size);
ST_FUNC void section_reserve(Section *sec, unsigned long size);
ST_FUNC Section *find_section(TCCState *s, const char *name);
ST_FUNC Section *new_symtab(TCCState *s, const char *name, int type, int flags,
                            const char *strtab, const char *hash, int hash_flags);
ST_FUNC void put_extern_sym2(Sym *sym, int sh_num, addr_t value, unsigned long size);
ST_FUNC void put_extern_sym(Sym *sym, Section *sec, addr_t value, unsigned long size);
#if PTR_SIZE == 4
ST_FUNC void greloc(Section *sec, Sym *sym, unsigned long offset, int type);
#endif
ST_FUNC void greloca(Section *sec, Sym *sym, unsigned long offset, int type, addr_t addend);
ST_FUNC int put_elf_str(Section *sec, const char *str);
ST_FUNC int put_elf_sym(Section *sec, addr_t value, unsigned long size,
                        int info, int other, int shndx, const char *name);
ST_FUNC int set_elf_sym(Section *sec, addr_t value, unsigned long size,
                        int info, int other, int shndx, const char *name);
ST_FUNC int find_elf_sym(Section *sec, const char *name);
ST_FUNC void put_elf_reloc(Section *symtab, Section *sec, unsigned long offset,
                           int type, int symbol);
ST_FUNC void put_elf_reloca(Section *symtab, Section *sec, unsigned long offset,
                            int type, int symbol, addr_t addend);
ST_FUNC void resolve_common_syms(TCCState *s);
ST_FUNC void relocate_syms(TCCState *s, Section *symtab);
ST_FUNC void relocate_section(TCCState *s, Section *sec);
ST_FUNC ssize_t full_read(int fd, void *buf, size_t count);
ST_FUNC void *load_data(int fd, unsigned long offset, unsigned long size);
ST_FUNC int tcc_object_type(int fd, ElfW(Ehdr) *h);
ST_FUNC int tcc_load_object_file(TCCState *s, int fd, unsigned long offset);
ST_FUNC int tcc_load_archive(TCCState *s, int fd, int alacarte);
ST_FUNC void add_array(TCCState *s, const char *sec, int c);
ST_FUNC void build_got_entries(TCCState *s);
struct sym_attr;
ST_FUNC struct sym_attr *get_sym_attr(TCCState *s, int index, int alloc);
ST_FUNC void squeeze_multi_relocs(Section *sec, size_t old_offset);
ST_FUNC addr_t get_sym_addr(TCCState *s, const char *name, int err, int forc);
ST_FUNC void list_elf_symbols(TCCState *s, void *ctx,
    void (*symbol_cb)(void *ctx, const char *name, const void *val));
ST_FUNC int set_global_sym(TCCState *s, const char *name, Section *sec, addr_t offs);
ST_FUNC void tcc_add_runtime(TCCState *s);

#define for_each_elem(sec, startoff, elem, type) \
    for (elem = (type *)(sec)->data + (startoff); \
         elem < (type *)((sec)->data + (sec)->data_offset); elem++)

#endif
