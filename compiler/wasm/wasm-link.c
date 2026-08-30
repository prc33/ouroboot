/* Adapted from Blosc/MiniCC's LGPL-2.1 WebAssembly backend:
   https://github.com/Blosc/minicc */
#ifdef TARGET_DEFS_ONLY
#ifndef EM_WEBASSEMBLY
#define EM_WEBASSEMBLY 0x4157
#endif
#define EM_TCC_TARGET EM_WEBASSEMBLY
/* wasm path does not use ELF relocations; keep placeholders for shared code. */
#define R_DATA_32   0
#define R_DATA_PTR  0
#define R_JMP_SLOT  0
#define R_GLOB_DAT  0
#define R_COPY      0
#define R_RELATIVE  0
#define R_NUM       1
#define ELF_START_ADDR 0
#define ELF_PAGE_SIZE  0x10000
#define PCRELATIVE_DLLPLT 0
#define RELOCATE_DLLPLT 0
#else /* !TARGET_DEFS_ONLY */
#include "../tcc.h"
ST_FUNC int code_reloc(int reloc_type)
{
    (void)reloc_type;
    return -1;
}
ST_FUNC int gotplt_entry_type(int reloc_type)
{
    (void)reloc_type;
    return NO_GOTPLT_ENTRY;
}
ST_FUNC unsigned create_plt_entry(TCCState *s1, unsigned got_offset, struct sym_attr *attr)
{
    (void)s1;
    (void)got_offset;
    (void)attr;
    tcc_error_noabort("wasm32 backend: GOT/PLT is not supported");
    return 0;
}
ST_FUNC void relocate_plt(TCCState *s1)
{
    (void)s1;
}
ST_FUNC void relocate(TCCState *s1, ElfW_Rel *rel, int type, unsigned char *ptr, addr_t addr, addr_t val)
{
    (void)s1;
    (void)rel;
    (void)type;
    (void)ptr;
    (void)addr;
    (void)val;
    tcc_error_noabort("wasm32 backend: ELF relocation is not supported");
}
#endif /* !TARGET_DEFS_ONLY */
