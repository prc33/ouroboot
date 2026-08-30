/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2001-2004 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _TCC_H
#define _TCC_H

#include "common.h"

# define IS_ABSPATH(p) IS_DIRSEP(p[0])
# define PATHCMP strcmp
# define PATHSEP ":"

/* -------------------------------------------- */

/* The build selects one of the three backends; retain i386 as the default. */
#if !defined(TCC_TARGET_I386) && !defined(TCC_TARGET_RISCV64) && \
    !defined(TCC_TARGET_WASM32)
# define TCC_TARGET_I386
#endif

/* (target specific) libtcc1.a */
#ifndef TCC_LIBTCC1
# define TCC_LIBTCC1 "libtcc1.a"
#endif

/* -------------------------------------------- */

#include "tccelf.h"

/* -------------------------------------------- */
/* include the target specific definitions */

#include "target.h"

/* -------------------------------------------- */

/* -------------------------------------------- */


typedef struct TCCState TCCState;

#define TCC_OUTPUT_EXE        2
#define TCC_OUTPUT_OBJ        4
#define TCC_OUTPUT_PREPROCESS 5

ST_FUNC TCCState *tcc_new(void);
ST_FUNC void tcc_delete(TCCState *s);
ST_FUNC void tcc_set_lib_path(TCCState *s, const char *path);
ST_FUNC int tcc_add_include_path(TCCState *s, const char *path);
ST_FUNC int tcc_add_sysinclude_path(TCCState *s, const char *path);
ST_FUNC void tcc_define_symbol(TCCState *s, const char *sym, const char *value);
ST_FUNC void tcc_undefine_symbol(TCCState *s, const char *sym);
ST_FUNC int tcc_add_file(TCCState *s, const char *filename);
ST_FUNC int tcc_set_output_type(TCCState *s, int output_type);
ST_FUNC int tcc_add_library_path(TCCState *s, const char *path);
ST_FUNC int tcc_add_library(TCCState *s, const char *library);
ST_FUNC int tcc_output_file(TCCState *s, const char *filename);

#include "utils.h"
#include "parsing.h"
#include "vstack.h"
#include "symbols.h"

/* -------------------------------------------------- */


/* type_decl() types */
#define TYPE_ABSTRACT  1 /* type without variable */
#define TYPE_DIRECT    2 /* type with variable */

/* inline functions */
typedef struct InlineFunc {
    TokenString *func_str;
    Sym *sym;
    char filename[1];
} InlineFunc;

/* include file cache, used to find files faster and also to eliminate
   inclusion if the include file is protected by #ifndef ... #endif */
typedef struct CachedInclude {
    int ifndef_macro;
    int once;
    int hash_next; /* -1 if none */
    char filename[1]; /* path specified in #include */
} CachedInclude;

#define CACHED_INCLUDES_HASH_SIZE 32

#ifdef CONFIG_TCC_ASM
typedef struct ExprValue {
    uint64_t v;
    Sym *sym;
    int pcrel;
} ExprValue;

#define MAX_ASM_OPERANDS 30
typedef struct ASMOperand {
    int id; /* GCC 3 optional identifier (0 if number only supported */
    char *constraint;
    char asm_str[16]; /* computed asm string for operand */
    SValue *vt; /* C value of the expression */
    int ref_index; /* if >= 0, gives reference to a output constraint */
    int input_index; /* if >= 0, gives reference to an input constraint */
    int priority; /* priority, used to assign registers */
    int reg; /* if >= 0, register number used for this operand */
    int is_llong; /* true if double register value */
    int is_memory; /* true if memory operand */
    int is_rw;     /* for '+' modifier */
} ASMOperand;
#endif

struct sym_attr {
    unsigned got_offset;
};

struct TCCState {
    unsigned char verbose; /* if true, display some information during compilation */
    unsigned char nostdinc; /* if true, no standard headers are added */
    unsigned char nostdlib; /* if true, no standard libraries are added */
    unsigned char nocommon; /* if true, do not use common symbols for .bss data */
    unsigned char filetype; /* file type for compilation (NONE,C,ASM) */
    unsigned char optimize; /* only to #define __OPTIMIZE__ */
    unsigned char option_pthread; /* -pthread option */
    unsigned int  cversion; /* supported C ISO version, 199901 (the default), 201112, ... */

    char *tcc_lib_path; /* CONFIG_TCCDIR or -B option */

    /* output type, see TCC_OUTPUT_XXX */
    int output_type;
    /* C language options */
    unsigned char char_is_unsigned;

    /* warning switches */
    unsigned char warn_error;
    unsigned char warn_none;
    unsigned char warn_implicit_function_declaration;

    /* compile with debug symbol (and use them if error during execution) */
    addr_t text_addr; /* address of text section */
    unsigned char has_text_addr;

    unsigned section_align; /* section alignment */

    /* include paths */
    char **include_paths;
    int nb_include_paths;

    char **sysinclude_paths;
    int nb_sysinclude_paths;

    /* library paths */
    char **library_paths;
    int nb_library_paths;

    /* crt?.o object path */
    char **crt_paths;
    int nb_crt_paths;

    /* -D / -U options */
    CString cmdline_defs;
    /* -include options */
    CString cmdline_incl;

    /* error handling (no caller-installed callback) */
    int error_set_jmp_enabled;
    jmp_buf error_jmp_buf;
    int nb_errors;

    /* output file for preprocessing (-E) */
    FILE *ppfp;
    enum {
	LINE_MACRO_OUTPUT_FORMAT_GCC,
	LINE_MACRO_OUTPUT_FORMAT_NONE,
	LINE_MACRO_OUTPUT_FORMAT_STD
    } Pflag; /* -P switch */

    /* for -MD/-MF: collected dependencies for this compilation */
    char **target_deps;
    int nb_target_deps;

    /* compilation */
    BufferedFile *include_stack[INCLUDE_STACK_SIZE];
    BufferedFile **include_stack_ptr;

    int ifdef_stack[IFDEF_STACK_SIZE];
    int *ifdef_stack_ptr;

    /* included files enclosed with #ifndef MACRO */
    int cached_includes_hash[CACHED_INCLUDES_HASH_SIZE];
    CachedInclude **cached_includes;
    int nb_cached_includes;

    /* #pragma pack stack */

    /* inline functions are stored as token lists and compiled last
       only if referenced */
    struct InlineFunc **inline_fns;
    int nb_inline_fns;

    /* sections */
    Section **sections;
    int nb_sections; /* number of sections, including first dummy section */

    Section **priv_sections;
    int nb_priv_sections; /* number of private sections */

    Section *got;

    /* predefined sections */
    Section *text_section, *data_section, *bss_section;
    Section *common_section;
    Section *cur_text_section; /* current section where function code is generated */
    /* symbol sections */
    Section *symtab_section;
    /* debug sections */
    /* copy of the global symtab_section variable */
    Section *symtab;
    struct sym_attr *sym_attrs;
    int nb_sym_attrs;
    /* ptr to next reloc entry reused */
    ElfW_Rel *qrel;
#   define qrel s1->qrel


    /* for warnings/errors for object files*/
    const char *current_filename;

    /* used by main and tcc_parse_args only */
    struct filespec **files; /* files seen on command line */
    int nb_files; /* number thereof */
    int nb_libraries; /* number of libs thereof */
    char *outfile; /* output filename */
    unsigned char option_r; /* option -r */
    int gen_deps; /* option -MD  */
    char *deps_outfile; /* option -MF */
};

struct filespec {
    char type;
    char name[1];
};

ST_DATA struct TCCState *tcc_state;

PUB_FUNC void _tcc_error_noabort(const char *fmt, ...) PRINTF_LIKE(1,2);
PUB_FUNC NORETURN void _tcc_error(const char *fmt, ...) PRINTF_LIKE(1,2);
PUB_FUNC void _tcc_warning(const char *fmt, ...) PRINTF_LIKE(1,2);

ST_FUNC void tcc_open_bf(TCCState *s1, const char *filename, int initlen);
ST_FUNC int tcc_open(TCCState *s1, const char *filename);
ST_FUNC void tcc_set_cur_filename(const char *filename);
ST_FUNC void tcc_close(void);

ST_FUNC int tcc_add_file_internal(TCCState *s1, const char *filename, int flags);
/* flags: */
#define AFF_PRINT_ERROR     0x10 /* print error if file not found */
#define AFF_TYPE_BIN        0x40 /* file to add is binary */
#define AFF_WHOLE_ARCHIVE   0x80 /* load all objects from archive */
/* s->filetype: */
#define AFF_TYPE_NONE   0
#define AFF_TYPE_C      1
#define AFF_TYPE_ASM    2
#define AFF_TYPE_ASMPP  4
#define AFF_TYPE_LIB    8
#define AFF_TYPE_MASK   (15 | AFF_TYPE_BIN)
/* values from tcc_object_type(...) */
#define AFF_BINTYPE_REL 1
#define AFF_BINTYPE_AR  3
ST_FUNC int tcc_add_crt(TCCState *s, const char *filename);
PUB_FUNC int tcc_add_library_err(TCCState *s, const char *f);
PUB_FUNC int tcc_parse_args(TCCState *s, int *argc, char ***argv, int optind);
ST_FUNC int tcc_tool_ar(int argc, char **argv);
ST_FUNC void gen_makedeps(TCCState *s, const char *target, const char *filename);

/* tcc_parse_args return codes: */
#define OPT_HELP 1
#define OPT_HELP2 2
#define OPT_V 3
#define OPT_AR 5

/* ------------ tccpp.c ------------ */

ST_FUNC int set_idnum(int c, int val);
ST_FUNC void begin_macro(TokenString *str, int alloc);
ST_FUNC void end_macro(void);
ST_INLN void define_push(int v, int macro_type, int *str, Sym *first_arg);
ST_FUNC void define_undef(Sym *s);
ST_INLN Sym *define_find(int v);
ST_FUNC void free_defines(Sym *b);
ST_FUNC Sym *label_find(int v);
ST_FUNC Sym *label_push(Sym **ptop, int v, int flags);
ST_FUNC void label_pop(Sym **ptop, Sym *slast, int keep);
ST_FUNC void parse_define(void);
ST_FUNC void preprocess(int is_bof);
ST_FUNC void next(void);
ST_INLN void unget_tok(int last_tok);
ST_FUNC void preprocess_start(TCCState *s1, int filetype);
ST_FUNC void preprocess_end(TCCState *s1);
ST_FUNC void tccpp_new(TCCState *s);
ST_FUNC void tccpp_delete(TCCState *s);
ST_FUNC int tcc_preprocess(TCCState *s1);
ST_FUNC void skip(int c);
ST_FUNC NORETURN void expect(const char *msg);

/* ------------ tccgen.c ------------ */

#define SYM_POOL_NB (8192 / sizeof(Sym))

ST_DATA Sym *global_stack;
ST_DATA Sym *local_stack;
ST_DATA Sym *global_label_stack;
ST_DATA Sym *define_stack;
ST_DATA CType int_type, func_old_type, char_pointer_type;
ST_DATA SValue *vtop;
ST_DATA int rsym, anon_sym, ind, loc;

ST_DATA int const_wanted; /* true if constant wanted */
ST_DATA int nocode_wanted; /* true if no code generation wanted for an expression */
ST_DATA int global_expr;  /* true if compound literals must be allocated globally (used during initializers parsing */
ST_DATA int local_scope;
ST_DATA CType func_vt; /* current function return type (used by return instruction) */
ST_DATA int func_var; /* true if current function is variadic */
ST_DATA int func_vc;
ST_DATA const char *funcname;

ST_FUNC void tccgen_init(TCCState *s1);
ST_FUNC int tccgen_compile(TCCState *s1);
ST_FUNC void tccgen_finish(TCCState *s1);
ST_FUNC void check_vstack(void);
#include "expr.h"
ST_FUNC int ieee_finite(double d);
ST_FUNC void test_lvalue(void);
ST_FUNC void vpushi(int v);
ST_FUNC void vpush64(int ty, unsigned long long v);
ST_FUNC void vpushs(addr_t v);
ST_FUNC void vpush_ref(CType *type, Section *sec, unsigned long offset, unsigned long size);
ST_FUNC void gen_cast(CType *type);
ST_FUNC void init_putv(CType *type, Section *sec, unsigned long c);
ST_FUNC ElfSym *elfsym(Sym *);
ST_FUNC void update_storage(Sym *sym);
ST_FUNC void vset(CType *type, int r, int v);
ST_FUNC void vset_VT_CMP(int op);
ST_FUNC void vset_VT_JMP(void);
ST_FUNC int RC_TYPE(int t);
ST_FUNC int RC2_TYPE(int t, int rc);
ST_FUNC void vswap(void);
ST_FUNC void vpush_global_sym(CType *type, int v);
ST_FUNC void vrote(SValue *e, int n);
ST_FUNC void vrott(int n);
ST_FUNC void vrotb(int n);
#include "registers.h"
#include "optimise.h"
ST_FUNC void vpop(void);
ST_FUNC void gen_op(int op);
ST_FUNC int type_size(CType *type, int *a);
ST_FUNC void mk_pointer(CType *type);
ST_FUNC void vstore(void);
ST_FUNC void inc(int post, int c);
ST_FUNC void parse_mult_str (CString *astr, const char *msg);
ST_FUNC void parse_asm_str(CString *astr);
ST_FUNC void indir(void);
ST_FUNC void unary(void);
ST_FUNC void gexpr(void);
ST_FUNC int expr_const(void);

/* ------------ tccasm.c ------------ */
ST_FUNC void asm_instr(void);
ST_FUNC void asm_global_instr(void);
#ifdef CONFIG_TCC_ASM
ST_FUNC int find_constraint(ASMOperand *operands, int nb_operands, const char *name, const char **pp);
ST_FUNC void asm_expr(TCCState *s1, ExprValue *pe);
ST_FUNC int asm_int_expr(TCCState *s1);
ST_FUNC int tcc_assemble(TCCState *s1, int do_preprocess);
/* ------------ i386-asm.c ------------ */
ST_FUNC void gen_expr32(ExprValue *pe);
ST_FUNC void asm_opcode(TCCState *s1, int opcode);
ST_FUNC int asm_parse_regvar(int t);
ST_FUNC void asm_compute_constraints(ASMOperand *operands, int nb_operands, int nb_outputs, const uint8_t *clobber_regs, int *pout_reg);
ST_FUNC void subst_asm_operand(CString *add_str, SValue *sv, int modifier);
ST_FUNC void asm_gen_code(ASMOperand *operands, int nb_operands, int nb_outputs, int is_output, uint8_t *clobber_regs, int out_reg);
ST_FUNC void asm_clobber(uint8_t *clobber_regs, const char *str);
#endif

#define ST_ASM_SET 0x04

/********************************************************/
#undef ST_DATA
#define ST_DATA
/********************************************************/

#define text_section        TCC_STATE_VAR(text_section)
#define data_section        TCC_STATE_VAR(data_section)
#define bss_section         TCC_STATE_VAR(bss_section)
#define common_section      TCC_STATE_VAR(common_section)
#define cur_text_section    TCC_STATE_VAR(cur_text_section)
#define symtab_section      TCC_STATE_VAR(symtab_section)
#define tcc_error_noabort   TCC_SET_STATE(_tcc_error_noabort)
#define tcc_error           TCC_SET_STATE(_tcc_error)
#define tcc_warning         TCC_SET_STATE(_tcc_warning)


PUB_FUNC void tcc_enter_state(TCCState *s1);
PUB_FUNC void tcc_exit_state(void);

/********************************************************/
#endif /* _TCC_H */

#undef TCC_STATE_VAR
#undef TCC_SET_STATE

#ifdef USING_GLOBALS
# define TCC_STATE_VAR(sym) tcc_state->sym
# define TCC_SET_STATE(fn) fn
# undef USING_GLOBALS
#else
# define TCC_STATE_VAR(sym) s1->sym
# define TCC_SET_STATE(fn) (tcc_enter_state(s1),fn)
/* actually we could avoid the tcc_enter_state(s1) hack by using
   __VA_ARGS__ except that some compiler doesn't support it. */
#endif
