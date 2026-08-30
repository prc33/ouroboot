#ifndef TCC_SYMBOLS_H
#define TCC_SYMBOLS_H

#include "types.h"

/* symbol attributes */
struct SymAttr {
    unsigned short
    aligned     : 5, /* alignment as log2+1 (0 == unspecified) */
    packed      : 1,
    weak        : 1,
    visibility  : 2,
    addrtaken   : 1,
    xxxx        : 6; /* not used */
};

/* function attributes or temporary attributes for parsing */
struct FuncAttr {
    unsigned
    func_type   : 2, /* FUNC_OLD/NEW/ELLIPSIS */
    func_noreturn : 1, /* attribute((noreturn)) */
    func_alwinl : 1, /* always_inline */
    xxxx        :23;
};

/* symbol management */
typedef struct Sym {
    int v; /* symbol token */
    unsigned short r; /* associated register or VT_CONST/VT_LOCAL and LVAL type */
    struct SymAttr a; /* symbol attributes */
    union {
        struct {
            int c; /* associated number or Elf symbol index */
            union {
                int sym_scope; /* scope level for locals */
                int jnext; /* next jump label */
                struct FuncAttr f; /* function attributes */
                int auxtype; /* bitfield access type */
            };
        };
        long long enum_val; /* enum constant if IS_ENUM_VAL */
        int *d; /* define token stream */
    };
    CType type; /* associated type */
    union {
        struct Sym *next; /* next related symbol (for fields and anoms) */
        int asm_label; /* associated asm label */
    };
    struct Sym *prev; /* prev symbol in stack */
    struct Sym *prev_tok; /* previous symbol for this token */
} Sym;

typedef struct AttributeDef {
    struct SymAttr a;
    struct FuncAttr f;
    struct Section *section;
    int asm_label;
} AttributeDef;
#define SYM_STRUCT     0x40000000 /* struct/union/enum symbol space */
#define SYM_FIELD      0x20000000 /* struct/union field symbol space */
#define SYM_FIRST_ANOM 0x10000000 /* first anonymous sym */

/* stored in 'Sym->f.func_type' field */
#define FUNC_NEW       1 /* ansi function prototype */
#define FUNC_OLD       2 /* old function prototype */
#define FUNC_ELLIPSIS  3 /* ansi function prototype with ... */

/* field 'Sym.t' for macros */
#define MACRO_OBJ      0 /* object like macro */
#define MACRO_FUNC     1 /* function like macro */

/* field 'Sym.r' for C labels */
#define LABEL_DEFINED  0 /* label is defined */
#define LABEL_FORWARD  1 /* label is forward defined */
#define LABEL_GONE     2 /* label is out of scope but retained by a statement expression */
ST_INLN void sym_free(Sym *sym);
ST_FUNC Sym *sym_malloc(void);
ST_FUNC Sym *sym_push2(Sym **ps, int v, int t, int c);
ST_FUNC Sym *sym_find2(Sym *s, int v);
ST_FUNC Sym *sym_push(int v, CType *type, int r, int c);
ST_FUNC void sym_pop(Sym **ptop, Sym *b, int keep);
ST_INLN Sym *struct_find(int v);
ST_INLN Sym *sym_find(int v);
ST_FUNC Sym *global_identifier_push(int v, int t, int c);
ST_FUNC Sym *asm_label_find(int v);
ST_FUNC Sym *asm_label_push(int v);
ST_FUNC Sym *get_asm_sym(int name, Sym *csym);
ST_FUNC Sym *external_global_sym(int v, CType *type);
ST_FUNC Sym *external_sym(int v, CType *type, int r, AttributeDef *ad);
ST_FUNC void patch_storage(Sym *sym, AttributeDef *ad, CType *type);
ST_FUNC void merge_symattr(struct SymAttr *to, struct SymAttr *from);
ST_FUNC void merge_funcattr(struct FuncAttr *to, struct FuncAttr *from);
ST_FUNC void symbols_finish(void);
#endif
