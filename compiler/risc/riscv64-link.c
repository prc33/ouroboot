#ifdef TARGET_DEFS_ONLY

#define EM_TCC_TARGET EM_RISCV

#define R_DATA_32  R_RISCV_32
#define R_DATA_PTR R_RISCV_64
#define R_GLOB_DAT R_RISCV_64
#define R_NUM      R_RISCV_NUM

#define ELF_START_ADDR 0x00010000
#define ELF_PAGE_SIZE 0x1000


#else /* !TARGET_DEFS_ONLY */

//#define DEBUG_RELOC
#include "../tcc.h"

int gotplt_entry_type (int reloc_type)
{
    return reloc_type == R_RISCV_GOT_HI20 ? ALWAYS_GOTPLT_ENTRY
                                          : NO_GOTPLT_ENTRY;
}

struct pcrel_hi {
    addr_t addr, val;
};
static struct pcrel_hi last_hi;

void relocate(TCCState *s1, ElfW_Rel *rel, int type, unsigned char *ptr,
              addr_t addr, addr_t val)
{
    uint64_t off64;
    uint32_t off32;
    int sym_index = ELFW(R_SYM)(rel->r_info);
    ElfW(Sym) *sym = &((ElfW(Sym) *)symtab_section->data)[sym_index];

    switch(type) {
    case R_RISCV_ALIGN:
    case R_RISCV_RELAX:
        return;

    case R_RISCV_BRANCH:
        off64 = val - addr;
        if ((off64 + (1 << 12)) & ~(uint64_t)0x1ffe)
          tcc_error("R_RISCV_BRANCH relocation failed"
                    " (val=%lx, addr=%lx)", val, addr);
        off32 = off64 >> 1;
        write32le(ptr, (read32le(ptr) & ~0xfe000f80)
                       | ((off32 & 0x800) << 20)
                       | ((off32 & 0x3f0) << 21)
                       | ((off32 & 0x00f) << 8)
                       | ((off32 & 0x400) >> 3));
        return;
    case R_RISCV_JAL:
        off64 = val - addr;
        if ((off64 + (1 << 21)) & ~(((uint64_t)1 << 22) - 2))
          tcc_error("R_RISCV_JAL relocation failed"
                    " (val=%lx, addr=%lx)", val, addr);
        off32 = off64;
        write32le(ptr, (read32le(ptr) & 0xfff)
                       | (((off32 >> 12) &  0xff) << 12)
                       | (((off32 >> 11) &     1) << 20)
                       | (((off32 >>  1) & 0x3ff) << 21)
                       | (((off32 >> 20) &     1) << 31));
        return;
    case R_RISCV_CALL:
    case R_RISCV_CALL_PLT:
        write32le(ptr, (read32le(ptr) & 0xfff)
                       | ((val - addr + 0x800) & ~0xfff));
        write32le(ptr + 4, (read32le(ptr + 4) & 0xfffff)
                           | (((val - addr) & 0xfff) << 20));
        return;
    case R_RISCV_PCREL_HI20:
        off64 = (int64_t)(val - addr + 0x800) >> 12;
        if ((off64 + ((uint64_t)1 << 20)) >> 21)
          tcc_error("R_RISCV_PCREL_HI20 relocation failed: off=%lx cond=%lx sym=%s",
                    off64, ((int64_t)(off64 + ((uint64_t)1 << 20)) >> 21),
                    symtab_section->link->data + sym->st_name);
        write32le(ptr, (read32le(ptr) & 0xfff)
                       | ((off64 & 0xfffff) << 12));
        last_hi.addr = addr;
        last_hi.val = val;
        return;
    case R_RISCV_GOT_HI20:
        val = s1->got->sh_addr + get_sym_attr(s1, sym_index, 0)->got_offset;
        off64 = (int64_t)(val - addr + 0x800) >> 12;
        if ((off64 + ((uint64_t)1 << 20)) >> 21)
          tcc_error("R_RISCV_GOT_HI20 relocation failed");
        last_hi.addr = addr;
        last_hi.val = val;
        write32le(ptr, (read32le(ptr) & 0xfff)
                       | ((off64 & 0xfffff) << 12));
        return;
    case R_RISCV_PCREL_LO12_I:
        if (val != last_hi.addr)
          tcc_error("unsupported hi/lo pcrel reloc scheme");
        val = last_hi.val;
        addr = last_hi.addr;
        write32le(ptr, (read32le(ptr) & 0xfffff)
                       | (((val - addr) & 0xfff) << 20));
        return;
    case R_RISCV_PCREL_LO12_S:
        if (val != last_hi.addr)
          tcc_error("unsupported hi/lo pcrel reloc scheme");
        val = last_hi.val;
        addr = last_hi.addr;
        off32 = val - addr;
        write32le(ptr, (read32le(ptr) & ~0xfe000f80)
                       | ((off32 & 0xfe0) << 20)
                       | ((off32 & 0x01f) << 7));
        return;

    case R_RISCV_RVC_BRANCH:
        off64 = val - addr;
        if ((off64 + (1 << 8)) & ~(uint64_t)0x1fe)
          tcc_error("R_RISCV_RVC_BRANCH relocation failed");
        off32 = off64;
        write16le(ptr, (read16le(ptr) & 0xe383)
                       | (((off32 >> 5) & 1) << 2) | (((off32 >> 1) & 3) << 3)
                       | (((off32 >> 6) & 3) << 5) | (((off32 >> 3) & 3) << 10)
                       | (((off32 >> 8) & 1) << 12));
        return;
    case R_RISCV_RVC_JUMP:
        off64 = val - addr;
        if ((off64 + (1 << 11)) & ~(uint64_t)0xffe)
          tcc_error("R_RISCV_RVC_JUMP relocation failed");
        off32 = off64;
        write16le(ptr, (read16le(ptr) & 0xe003)
                       | (((off32 >> 5) & 1) << 2) | (((off32 >> 1) & 7) << 3)
                       | (((off32 >> 7) & 1) << 6) | (((off32 >> 6) & 1) << 7)
                       | (((off32 >> 10) & 1) << 8) | (((off32 >> 8) & 3) << 9)
                       | (((off32 >> 4) & 1) << 11) | (((off32 >> 11) & 1) << 12));
        return;

    case R_RISCV_32:
        add32le(ptr, val);
        return;
    case R_RISCV_64:
    case R_RISCV_JUMP_SLOT:
        add64le(ptr, val);
        return;
    case R_RISCV_ADD64: write64le(ptr, read64le(ptr) + val); return;
    case R_RISCV_ADD32: write32le(ptr, read32le(ptr) + val); return;
    case R_RISCV_SUB64: write64le(ptr, read64le(ptr) - val); return;
    case R_RISCV_SUB32: write32le(ptr, read32le(ptr) - val); return;
    case R_RISCV_ADD16: write16le(ptr, read16le(ptr) + val); return;
    case R_RISCV_SUB16: write16le(ptr, read16le(ptr) - val); return;
    case R_RISCV_SET6: *ptr = (*ptr & ~0x3f) | (val & 0x3f); return;
    case R_RISCV_SUB6: *ptr = (*ptr & ~0x3f) | ((*ptr - val) & 0x3f); return;
    case R_RISCV_32_PCREL:
    case R_RISCV_COPY:
        return;

    default:
        fprintf(stderr, "FIXME: handle reloc type %x at %x [%p] to %x\n",
                type, (unsigned)addr, ptr, (unsigned)val);
        return;
    }
}
#endif
