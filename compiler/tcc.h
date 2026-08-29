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
/* XXX: need to define this to use them in non ISOC99 context */
extern float strtof (const char *__nptr, char **__endptr);
extern long double strtold (const char *__nptr, char **__endptr);


# define IS_ABSPATH(p) IS_DIRSEP(p[0])
# define PATHCMP strcmp
# define PATHSEP ":"

/* -------------------------------------------- */

/* parser debug */
/* #define PARSE_DEBUG */
/* preprocessor debug */
/* #define PP_DEBUG */
/* include file debug */
/* #define INC_DEBUG */
/* memory leak debug (only for single threaded usage) */
/* assembler debug */
/* #define ASM_DEBUG */

/* The build selects one of the three backends; retain i386 as the default. */
#if !defined(TCC_TARGET_I386) && !defined(TCC_TARGET_RISCV64) && \
    !defined(TCC_TARGET_WASM32)
# define TCC_TARGET_I386
#endif

/* ------------ path configuration ------------ */

#ifndef CONFIG_SYSROOT
# define CONFIG_SYSROOT ""
#endif
#ifndef CONFIG_TCCDIR
# define CONFIG_TCCDIR "/usr/local/lib/tcc"
#endif
#ifndef CONFIG_LDDIR
# define CONFIG_LDDIR "lib"
#endif
#ifdef CONFIG_TRIPLET
# define USE_TRIPLET(s) s "/" CONFIG_TRIPLET
# define ALSO_TRIPLET(s) USE_TRIPLET(s) ":" s
#else
# define USE_TRIPLET(s) s
# define ALSO_TRIPLET(s) s
#endif

/* path to find crt1.o, crti.o and crtn.o */
#ifndef CONFIG_TCC_CRTPREFIX
# define CONFIG_TCC_CRTPREFIX USE_TRIPLET(CONFIG_SYSROOT "/usr/" CONFIG_LDDIR)
#endif

#ifndef CONFIG_USR_INCLUDE
# define CONFIG_USR_INCLUDE "/usr/include"
#endif

/* Below: {B} is substituted by CONFIG_TCCDIR (rsp. -B option) */

/* system include paths */
#ifndef CONFIG_TCC_SYSINCLUDEPATHS
#  define CONFIG_TCC_SYSINCLUDEPATHS \
        "{B}/include" \
    ":" ALSO_TRIPLET(CONFIG_SYSROOT "/usr/local/include") \
    ":" ALSO_TRIPLET(CONFIG_SYSROOT CONFIG_USR_INCLUDE)
#endif

/* library search paths */
#ifndef CONFIG_TCC_LIBPATHS
#  define CONFIG_TCC_LIBPATHS \
        ALSO_TRIPLET(CONFIG_SYSROOT "/usr/" CONFIG_LDDIR) \
    ":" ALSO_TRIPLET(CONFIG_SYSROOT "/" CONFIG_LDDIR) \
    ":" ALSO_TRIPLET(CONFIG_SYSROOT "/usr/local/" CONFIG_LDDIR)
#endif

/* name of ELF interpreter */
#ifndef CONFIG_TCC_ELFINTERP
# if defined __DragonFly__
#  define CONFIG_TCC_ELFINTERP "/usr/libexec/ld-elf.so.2"
# elif defined __GNU__
#  define CONFIG_TCC_ELFINTERP "/lib/ld.so"
# elif defined(TCC_TARGET_RISCV64)
#  define CONFIG_TCC_ELFINTERP "/lib/ld-linux-riscv64-lp64d.so.1"
# else
#  if defined(TCC_MUSL)
#   if defined(TCC_TARGET_I386)
#     define CONFIG_TCC_ELFINTERP "/lib/ld-musl-i386.so.1"
#    else
#     define CONFIG_TCC_ELFINTERP "/lib/ld-musl-arm.so.1"
#    endif
#  else
#   define CONFIG_TCC_ELFINTERP "/lib/ld-linux.so.2"
#  endif
# endif
#endif

/* var elf_interp dans *-gen.c */
#ifdef CONFIG_TCC_ELFINTERP
# define DEFAULT_ELFINTERP(s) CONFIG_TCC_ELFINTERP
#else
# define DEFAULT_ELFINTERP(s) default_elfinterp(s)
#endif

/* (target specific) libtcc1.a */
#ifndef TCC_LIBTCC1
# define TCC_LIBTCC1 "libtcc1.a"
#endif

/* -------------------------------------------- */

#include "tccelf.h"

#ifdef TCC_PROFILE /* profile all functions */
# define static
#endif

/* -------------------------------------------- */
/* include the target specific definitions */

#include "target.h"

/* -------------------------------------------- */

#if PTR_SIZE == 8
# define ElfW_Rel ElfW(Rela)
# define SHT_RELX SHT_RELA
# define REL_SECTION_FMT ".rela%s"
#else
# define ElfW_Rel ElfW(Rel)
# define SHT_RELX SHT_REL
# define REL_SECTION_FMT ".rel%s"
#endif
/* target address type */
#define addr_t ElfW(Addr)
#define ElfSym ElfW(Sym)

#if PTR_SIZE == 8
# define LONG_SIZE 8
#else
# define LONG_SIZE 4
#endif

/* -------------------------------------------- */


typedef struct TCCState TCCState;

#define TCC_OUTPUT_EXE        2
#define TCC_OUTPUT_OBJ        4
#define TCC_OUTPUT_PREPROCESS 5

ST_FUNC TCCState *tcc_new(void);
ST_FUNC void tcc_delete(TCCState *s);
ST_FUNC void tcc_set_lib_path(TCCState *s, const char *path);
ST_FUNC void tcc_set_options(TCCState *s, const char *str);
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

/* section definition */
typedef struct Section {
    unsigned long data_offset; /* current data offset */
    unsigned char *data;       /* section data */
    unsigned long data_allocated; /* used for realloc() handling */
    TCCState *s1;
    int sh_name;             /* elf section name (only used during output) */
    int sh_num;              /* elf section number */
    int sh_type;             /* elf section type */
    int sh_flags;            /* elf section flags */
    int sh_info;             /* elf section info */
    int sh_addralign;        /* elf section alignment */
    int sh_entsize;          /* elf entry size */
    unsigned long sh_size;   /* section size (only used during output) */
    addr_t sh_addr;          /* address at which the section is relocated */
    unsigned long sh_offset; /* file offset */
    int nb_hashed_syms;      /* used to resize the hash table */
    struct Section *link;    /* link to another section */
    struct Section *reloc;   /* corresponding section for relocation, if any */
    struct Section *hash;    /* hash table for symbols */
    struct Section *prev;    /* previous section on section stack */
    char name[1];           /* section name */
} Section;

typedef struct DLLReference {
    int level;
    void *handle;
    char name[1];
} DLLReference;

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

/* extra symbol attributes (not in symbol table) */
struct sym_attr {
    unsigned got_offset;
    unsigned plt_offset;
    int plt_sym;
    int dyn_index;
};

struct TCCState {
    unsigned char verbose; /* if true, display some information during compilation */
    unsigned char nostdinc; /* if true, no standard headers are added */
    unsigned char nostdlib; /* if true, no standard libraries are added */
    unsigned char nocommon; /* if true, do not use common symbols for .bss data */
    unsigned char static_link; /* if true, static linking is performed */
    unsigned char symbolic; /* if true, resolve symbols in the current module first */
    unsigned char filetype; /* file type for compilation (NONE,C,ASM) */
    unsigned char optimize; /* only to #define __OPTIMIZE__ */
    unsigned char option_pthread; /* -pthread option */
    unsigned char enable_new_dtags; /* -Wl,--enable-new-dtags */
    unsigned int  cversion; /* supported C ISO version, 199901 (the default), 201112, ... */

    char *tcc_lib_path; /* CONFIG_TCCDIR or -B option */
    char *rpath; /* as specified on the command line (-Wl,-rpath=) */

    /* output type, see TCC_OUTPUT_XXX */
    int output_type;
    /* C language options */
    unsigned char char_is_unsigned;
    unsigned char ms_extensions; /* allow nested named struct w/o identifier behave like unnamed */
    unsigned char dollars_in_identifiers;  /* allows '$' char in identifiers */
    unsigned char ms_bitfields; /* if true, emulate MS algorithm for aligning bitfields */

    /* warning switches */
    unsigned char warn_write_strings;
    unsigned char warn_unsupported;
    unsigned char warn_error;
    unsigned char warn_none;
    unsigned char warn_implicit_function_declaration;
    unsigned char warn_gcc_compat;

    /* compile with debug symbol (and use them if error during execution) */
    addr_t text_addr; /* address of text section */
    unsigned char has_text_addr;

    unsigned section_align; /* section alignment */

    /* use GNU C extensions */
    unsigned char gnu_ext;
    /* use TinyCC extensions */
    unsigned char tcc_ext;

    char *init_symbol; /* symbols to call at load-time (not used currently) */
    char *fini_symbol; /* symbols to call at unload-time (not used currently) */

#ifdef TCC_TARGET_I386
    int seg_size; /* 32. Can be 16 with i386 assembler (.code16) */
#endif

    /* array of all loaded dlls (including those referenced by loaded dlls) */
    DLLReference **loaded_dlls;
    int nb_loaded_dlls;

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
	LINE_MACRO_OUTPUT_FORMAT_STD,
    LINE_MACRO_OUTPUT_FORMAT_P10 = 11
    } Pflag; /* -P switch */
    char dflag; /* -dX value */

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
    int pack_stack[PACK_STACK_SIZE];
    int *pack_stack_ptr;
    char **pragma_libs;
    int nb_pragma_libs;

    /* inline functions are stored as token lists and compiled last
       only if referenced */
    struct InlineFunc **inline_fns;
    int nb_inline_fns;

    /* sections */
    Section **sections;
    int nb_sections; /* number of sections, including first dummy section */

    Section **priv_sections;
    int nb_priv_sections; /* number of private sections */

    /* got & plt handling */
    Section *got;
    Section *plt;

    /* predefined sections */
    Section *text_section, *data_section, *bss_section;
    Section *common_section;
    Section *cur_text_section; /* current section where function code is generated */
    /* symbol sections */
    Section *symtab_section;
    /* debug sections */
    /* Is there a new undefined sym since last new_undef_sym() */
    int new_undef_sym;

    /* temporary dynamic symbol sections (for dll loading) */
    Section *dynsymtab_section;
    /* exported dynamic symbol section */
    Section *dynsym;
    /* copy of the global symtab_section variable */
    Section *symtab;
    /* extra attributes (eg. GOT/PLT value) for symtab symbols */
    struct sym_attr *sym_attrs;
    int nb_sym_attrs;
    /* ptr to next reloc entry reused */
    ElfW_Rel *qrel;
#   define qrel s1->qrel


    int nb_sym_versions;
    struct sym_version *sym_versions;
    int nb_sym_to_version;
    int *sym_to_version;
    int dt_verneednum;
    Section *versym_section;
    Section *verneed_section;

    int fd, cc; /* used by tcc_load_ldscript */

    /* benchmark info */
    int total_idents;
    int total_lines;
    int total_bytes;

    /* option -dnum (for general development purposes) */
    int g_debug;

    /* for warnings/errors for object files*/
    const char *current_filename;

    /* used by main and tcc_parse_args only */
    struct filespec **files; /* files seen on command line */
    int nb_files; /* number thereof */
    int nb_libraries; /* number of libs thereof */
    char *outfile; /* output filename */
    unsigned char option_r; /* option -r */
    unsigned char do_bench; /* option -bench */
    int gen_deps; /* option -MD  */
    char *deps_outfile; /* option -MF */
    int argc;
    char **argv;
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
#define AFF_BINTYPE_DYN 2
#define AFF_BINTYPE_AR  3
ST_FUNC int tcc_add_crt(TCCState *s, const char *filename);
ST_FUNC void tcc_add_pragma_libs(TCCState *s1);
PUB_FUNC int tcc_add_library_err(TCCState *s, const char *f);
PUB_FUNC void tcc_print_stats(TCCState *s, unsigned total_time);
PUB_FUNC int tcc_parse_args(TCCState *s, int *argc, char ***argv, int optind);
ST_FUNC int tcc_tool_ar(int argc, char **argv);
ST_FUNC void gen_makedeps(TCCState *s, const char *target, const char *filename);

/* tcc_parse_args return codes: */
#define OPT_HELP 1
#define OPT_HELP2 2
#define OPT_V 3
#define OPT_PRINT_DIRS 4
#define OPT_AR 5

/* ------------ tccpp.c ------------ */

ST_DATA struct BufferedFile *file;
ST_DATA int ch, tok;
ST_DATA CValue tokc;
ST_DATA const int *macro_ptr;
ST_DATA int parse_flags;
ST_DATA int tok_flags;
ST_DATA CString tokcstr; /* current parsed string, if any */

/* display benchmark infos */
ST_DATA int tok_ident;
ST_DATA TokenSym **table_ident;

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
ST_DATA Sym *local_label_stack;
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

ST_FUNC void tcc_debug_funcstart(TCCState *s1, Sym *sym);
ST_FUNC void tcc_debug_funcend(TCCState *s1, int size);
ST_FUNC void tcc_debug_line(TCCState *s1);

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

/* ------------ tccelf.c ------------ */

#define ARMAG  "!<arch>\012"    /* Unix archive magic */

typedef struct {
    unsigned int n_strx;         /* index into string table of name */
    unsigned char n_type;         /* type of symbol */
    unsigned char n_other;        /* misc info (usually empty) */
    unsigned short n_desc;        /* description field */
    unsigned int n_value;        /* value of symbol */
} Stab_Sym;

ST_FUNC void tccelf_new(TCCState *s);
ST_FUNC void tccelf_delete(TCCState *s);
ST_FUNC void tccelf_begin_file(TCCState *s1);
ST_FUNC void tccelf_end_file(TCCState *s1);
ST_FUNC Section *new_section(TCCState *s1, const char *name, int sh_type, int sh_flags);
ST_FUNC void section_realloc(Section *sec, unsigned long new_size);
ST_FUNC size_t section_add(Section *sec, addr_t size, int align);
ST_FUNC void *section_ptr_add(Section *sec, addr_t size);
ST_FUNC void section_reserve(Section *sec, unsigned long size);
ST_FUNC Section *find_section(TCCState *s1, const char *name);
ST_FUNC Section *new_symtab(TCCState *s1, const char *symtab_name, int sh_type, int sh_flags, const char *strtab_name, const char *hash_name, int hash_sh_flags);

ST_FUNC void put_extern_sym2(Sym *sym, int sh_num, addr_t value, unsigned long size);
ST_FUNC void put_extern_sym(Sym *sym, Section *section, addr_t value, unsigned long size);
#if PTR_SIZE == 4
ST_FUNC void greloc(Section *s, Sym *sym, unsigned long offset, int type);
#endif
ST_FUNC void greloca(Section *s, Sym *sym, unsigned long offset, int type, addr_t addend);

ST_FUNC int put_elf_str(Section *s, const char *sym);
ST_FUNC int put_elf_sym(Section *s, addr_t value, unsigned long size, int info, int other, int shndx, const char *name);
ST_FUNC int set_elf_sym(Section *s, addr_t value, unsigned long size, int info, int other, int shndx, const char *name);
ST_FUNC int find_elf_sym(Section *s, const char *name);
ST_FUNC void put_elf_reloc(Section *symtab, Section *s, unsigned long offset, int type, int symbol);
ST_FUNC void put_elf_reloca(Section *symtab, Section *s, unsigned long offset, int type, int symbol, addr_t addend);


ST_FUNC void resolve_common_syms(TCCState *s1);
ST_FUNC void relocate_syms(TCCState *s1, Section *symtab);
ST_FUNC void relocate_section(TCCState *s1, Section *s);

ST_FUNC ssize_t full_read(int fd, void *buf, size_t count);
ST_FUNC void *load_data(int fd, unsigned long file_offset, unsigned long size);
ST_FUNC int tcc_object_type(int fd, ElfW(Ehdr) *h);
ST_FUNC int tcc_load_object_file(TCCState *s1, int fd, unsigned long file_offset);
ST_FUNC int tcc_load_archive(TCCState *s1, int fd, int alacarte);
ST_FUNC void add_array(TCCState *s1, const char *sec, int c);

ST_FUNC void build_got_entries(TCCState *s1);
ST_FUNC struct sym_attr *get_sym_attr(TCCState *s1, int index, int alloc);
ST_FUNC void squeeze_multi_relocs(Section *sec, size_t oldrelocoffset);

ST_FUNC addr_t get_sym_addr(TCCState *s, const char *name, int err, int forc);
ST_FUNC void list_elf_symbols(TCCState *s, void *ctx,
    void (*symbol_cb)(void *ctx, const char *name, const void *val));
ST_FUNC int set_global_sym(TCCState *s1, const char *name, Section *sec, addr_t offs);

/* Browse each elem of type <type> in section <sec> starting at elem <startoff>
   using variable <elem> */
#define for_each_elem(sec, startoff, elem, type) \
    for (elem = (type *) sec->data + startoff; \
         elem < (type *) (sec->data + sec->data_offset); elem++)

ST_FUNC void tcc_add_runtime(TCCState *s1);

/* ------------ xxx-link.c ------------ */

/* Whether to generate a GOT/PLT entry and when. NO_GOTPLT_ENTRY is first so
   that unknown relocation don't create a GOT or PLT entry */
enum gotplt_entry {
    NO_GOTPLT_ENTRY,	/* never generate (eg. GLOB_DAT & JMP_SLOT relocs) */
    BUILD_GOT_ONLY,	/* only build GOT (eg. TPOFF relocs) */
    AUTO_GOTPLT_ENTRY,	/* generate if sym is UNDEF */
    ALWAYS_GOTPLT_ENTRY	/* always generate (eg. PLTOFF relocs) */
};

ST_FUNC int code_reloc (int reloc_type);
ST_FUNC int gotplt_entry_type (int reloc_type);
ST_FUNC unsigned create_plt_entry(TCCState *s1, unsigned got_offset, struct sym_attr *attr);
ST_FUNC void relocate_plt(TCCState *s1);
ST_FUNC void relocate(TCCState *s1, ElfW_Rel *rel, int type, unsigned char *ptr, addr_t addr, addr_t val);

/* ------------ xxx-gen.c ------------ */

ST_DATA const int reg_classes[NB_REGS];

ST_FUNC void gsym_addr(int t, int a);
ST_FUNC void gsym(int t);
ST_FUNC void load(int r, SValue *sv);
ST_FUNC void store(int r, SValue *v);
ST_FUNC int gfunc_sret(CType *vt, int variadic, CType *ret, int *align, int *regsize);
ST_FUNC void gfunc_call(int nb_args);
ST_FUNC void gfunc_prolog(Sym *func_sym);
ST_FUNC void gfunc_epilog(void);
ST_FUNC void gen_fill_nops(int);
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
    p[0] = x & 255;  p[1] = x >> 8 & 255;
}
static inline uint32_t read32le(unsigned char *p) {
  return read16le(p) | (uint32_t)read16le(p + 2) << 16;
}
static inline void write32le(unsigned char *p, uint32_t x) {
    write16le(p, x);  write16le(p + 2, x >> 16);
}
static inline void add32le(unsigned char *p, int32_t x) {
    write32le(p, read32le(p) + x);
}
static inline uint64_t read64le(unsigned char *p) {
  return read32le(p) | (uint64_t)read32le(p + 4) << 32;
}
static inline void write64le(unsigned char *p, uint64_t x) {
    write32le(p, x);  write32le(p + 4, x >> 32);
}
static inline void add64le(unsigned char *p, int64_t x) {
    write64le(p, read64le(p) + x);
}

/* ------------ i386-gen.c ------------ */
#ifdef TCC_TARGET_I386
ST_FUNC void lexpand(void);
ST_FUNC void i386_gv_dup_llong(int t);
ST_FUNC int i386_gen_cvt_i64(int dbt, int sbt, int ds, int ss);
ST_FUNC void gen_opl(int op);
ST_FUNC void vdup(void);
ST_FUNC void gv_dup(void);
ST_FUNC int gvtst(int inv, int t);
ST_FUNC void gvtst_set(int inv, int t);
#endif
#if defined TCC_TARGET_I386
ST_FUNC void g(int c);
ST_FUNC void gen_le16(int c);
ST_FUNC void gen_le32(int c);
ST_FUNC void gen_addr32(int r, Sym *sym, int c);
ST_FUNC void gen_addrpc32(int r, Sym *sym, int c);
ST_FUNC void gen_cvt_csti(int t);
#endif

/* ------------ riscv64-gen.c ------------ */
ST_FUNC void riscv_gen_alloca(void);
#ifdef TCC_TARGET_RISCV64
ST_FUNC void gen_opl(int op);
//ST_FUNC void gfunc_return(CType *func_type);
ST_FUNC void gen_va_start(void);
ST_FUNC void arch_transfer_ret_regs(int);
ST_FUNC void gen_cvt_sxtw(void);
#endif
#ifdef TCC_NATIVE_I64
ST_FUNC void gen_opl(int op);
ST_FUNC void gen_cvt_i32_i64(int is_unsigned);
ST_FUNC void gen_cvt_i64_i32(void);
#endif

/* ------------ c67-gen.c ------------ */

/* ------------ tcccoff.c ------------ */

#ifdef TCC_TARGET_WASM32
ST_FUNC int tcc_output_wasm(TCCState *s1, const char *filename);
ST_FUNC void tcc_wasm_reset(void);
#endif

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
#define gnu_ext             TCC_STATE_VAR(gnu_ext)
#define tcc_error_noabort   TCC_SET_STATE(_tcc_error_noabort)
#define tcc_error           TCC_SET_STATE(_tcc_error)
#define tcc_warning         TCC_SET_STATE(_tcc_warning)

#define total_idents        TCC_STATE_VAR(total_idents)
#define total_lines         TCC_STATE_VAR(total_lines)
#define total_bytes         TCC_STATE_VAR(total_bytes)

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
