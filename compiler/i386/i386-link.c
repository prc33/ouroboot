#ifdef TARGET_DEFS_ONLY

#define EM_TCC_TARGET EM_386

/* relocation type for 32 bit data relocation */
#define R_DATA_32   R_386_32
#define R_DATA_PTR  R_386_32
#define R_GLOB_DAT  R_386_GLOB_DAT
#define R_NUM       R_386_NUM

#define ELF_START_ADDR 0x08048000
#define ELF_PAGE_SIZE  0x1000


#else /* !TARGET_DEFS_ONLY */

#include "../tcc.h"

int gotplt_entry_type (int reloc_type)
{
    switch (reloc_type) {
    case R_386_GOTPC:
    case R_386_GOTOFF:
        return BUILD_GOT_ONLY;
    case R_386_GOT32:
    case R_386_GOT32X:
        return ALWAYS_GOTPLT_ENTRY;
    }
    return NO_GOTPLT_ENTRY;
}

void relocate(TCCState *s1, ElfW_Rel *rel, int type, unsigned char *ptr, addr_t addr, addr_t val)
{
    int sym_index = ELFW(R_SYM)(rel->r_info);
    switch (type) {
        case R_386_32:
            add32le(ptr, val);
            return;
        case R_386_PC32:
            add32le(ptr, val - addr);
            return;
        case R_386_PLT32:
            add32le(ptr, val - addr);
            return;
        case R_386_GLOB_DAT:
        case R_386_JMP_SLOT:
            write32le(ptr, val);
            return;
        case R_386_GOTPC:
            add32le(ptr, s1->got->sh_addr - addr);
            return;
        case R_386_GOTOFF:
            add32le(ptr, val - s1->got->sh_addr);
            return;
        case R_386_GOT32:
        case R_386_GOT32X:
            add32le(ptr, get_sym_attr(s1, sym_index, 0)->got_offset);
            return;
        case R_386_RELATIVE:
        case R_386_COPY:
            return;
        default:
            fprintf(stderr,"FIXME: handle reloc type %d at %x [%p] to %x\n",
                type, (unsigned)addr, ptr, (unsigned)val);
            return;
    }
}

#endif /* !TARGET_DEFS_ONLY */
