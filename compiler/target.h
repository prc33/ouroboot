#ifndef TCC_TARGET_H
#define TCC_TARGET_H

#include "common.h"

#define TARGET_DEFS_ONLY
#ifdef TCC_TARGET_I386
# include "i386/i386-gen.c"
# include "i386/i386-link.c"
#elif defined(TCC_TARGET_RISCV64)
# include "risc/riscv64-gen.c"
# include "risc/riscv64-link.c"
# include "risc/riscv64-asm.c"
#elif defined(TCC_TARGET_WASM32)
# include "wasm/wasm-gen.c"
# include "wasm/wasm-link.c"
#else
# error unknown target
#endif
#undef TARGET_DEFS_ONLY

#include "elf.h"
#if PTR_SIZE == 8
# define ElfW(type) Elf##64##_##type
# define ElfW_Rel ElfW(Rela)
# define SHT_RELX SHT_RELA
# define REL_SECTION_FMT ".rela%s"
#else
# define ElfW(type) Elf##32##_##type
# define ElfW_Rel ElfW(Rel)
# define SHT_RELX SHT_REL
# define REL_SECTION_FMT ".rel%s"
#endif
#define addr_t ElfW(Addr)
#define ElfSym ElfW(Sym)
#if PTR_SIZE == 8
# define LONG_SIZE 8
#else
# define LONG_SIZE 4
#endif

typedef struct TCCState TCCState;
typedef struct CType CType;
typedef struct SValue SValue;
typedef struct Sym Sym;
struct sym_attr;

ST_FUNC void vpushi(int v);
ST_FUNC void vpushll(long long v);
ST_FUNC void vdup(void);
ST_FUNC void gen_op(int op);
ST_FUNC int gvtst(int inv, int t);

enum gotplt_entry {
    NO_GOTPLT_ENTRY,
    BUILD_GOT_ONLY,
    AUTO_GOTPLT_ENTRY,
    ALWAYS_GOTPLT_ENTRY
};

ST_FUNC int code_reloc(int reloc_type);
ST_FUNC int gotplt_entry_type(int reloc_type);
ST_FUNC unsigned create_plt_entry(TCCState *s, unsigned got_offset,
                                  struct sym_attr *attr);
ST_FUNC void relocate_plt(TCCState *s);
ST_FUNC void relocate(TCCState *s, ElfW_Rel *rel, int type,
                      unsigned char *ptr, addr_t addr, addr_t val);

ST_DATA const int reg_classes[NB_REGS];
ST_FUNC void gsym_addr(int t, int a);
ST_FUNC void gsym(int t);
ST_FUNC void load(int r, SValue *sv);
ST_FUNC void store(int r, SValue *v);
ST_FUNC int gfunc_sret(CType *vt, int variadic, CType *ret,
                      int *align, int *regsize);
ST_FUNC void gfunc_call(int nb_args);
ST_FUNC void gfunc_prolog(Sym *func_sym);
ST_FUNC void gfunc_epilog(void);
ST_FUNC void gen_fill_nops(int n);
ST_FUNC int gjmp(int t);
ST_FUNC void gjmp_addr(int a);
ST_FUNC int gjmp_cond(int op, int t);
ST_FUNC int gjmp_append(int n, int t);
ST_FUNC void gen_opi(int op);
ST_FUNC void gen_opf(int op);
ST_FUNC void gen_cvt_ftoi(int t);
ST_FUNC void gen_cvt_itof(int t);
ST_FUNC void gen_cvt_ftof(int t);
ST_FUNC void ggoto(void);
ST_FUNC void o(unsigned int c);
ST_FUNC void gen_vla_sp_save(int addr);
ST_FUNC void gen_vla_sp_restore(int addr);
ST_FUNC void gen_vla_alloc(CType *type, int align);

static inline uint16_t read16le(unsigned char *p) {
    return p[0] | (uint16_t)p[1] << 8;
}
static inline void write16le(unsigned char *p, uint16_t x) {
    p[0] = x; p[1] = x >> 8;
}
static inline uint32_t read32le(unsigned char *p) {
    return read16le(p) | (uint32_t)read16le(p + 2) << 16;
}
static inline void write32le(unsigned char *p, uint32_t x) {
    write16le(p, x); write16le(p + 2, x >> 16);
}
static inline void add32le(unsigned char *p, int32_t x) {
    write32le(p, read32le(p) + x);
}
static inline uint64_t read64le(unsigned char *p) {
    return read32le(p) | (uint64_t)read32le(p + 4) << 32;
}
static inline void write64le(unsigned char *p, uint64_t x) {
    write32le(p, x); write32le(p + 4, x >> 32);
}
static inline void add64le(unsigned char *p, int64_t x) {
    write64le(p, read64le(p) + x);
}

#ifdef TCC_TARGET_I386
ST_FUNC void lexpand(void);
ST_FUNC void i386_gv_dup_llong(int t);
ST_FUNC int i386_gen_cvt_i64(int dbt, int sbt, int ds, int ss);
ST_FUNC void gen_opl(int op);
ST_FUNC void vdup(void);
ST_FUNC void gv_dup(void);
ST_FUNC int gvtst(int inv, int t);
ST_FUNC void gvtst_set(int inv, int t);
ST_FUNC void g(int c);
ST_FUNC void gen_le16(int c);
ST_FUNC void gen_le32(int c);
ST_FUNC void gen_addr32(int r, Sym *sym, int c);
ST_FUNC void gen_addrpc32(int r, Sym *sym, int c);
ST_FUNC void gen_cvt_csti(int t);
#elif defined TCC_TARGET_RISCV64
ST_FUNC void riscv_gen_alloca(void);
ST_FUNC void gen_opl(int op);
ST_FUNC void gen_va_start(void);
ST_FUNC void arch_transfer_ret_regs(int n);
ST_FUNC void gen_cvt_sxtw(void);
#elif defined TCC_TARGET_WASM32
ST_FUNC void gen_opl(int op);
ST_FUNC void gen_cvt_i32_i64(int is_unsigned);
ST_FUNC void gen_cvt_i64_i32(void);
ST_FUNC int tcc_output_wasm(TCCState *s, const char *filename);
ST_FUNC void tcc_wasm_reset(void);
#endif

#endif
