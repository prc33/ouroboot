
#define USING_GLOBALS
#include "tcc.h"
#include "symbols.h"

static Sym *sym_free_first;
static void **sym_pools;
static int nb_sym_pools;

ST_FUNC ElfSym *elfsym(Sym *s)
{
  if (!s || !s->c)
    return NULL;
  return &((ElfSym *)symtab_section->data)[s->c];
}

/* apply storage attributes to Elf symbol */
ST_FUNC void update_storage(Sym *sym)
{
    ElfSym *esym;
    int sym_bind, old_sym_bind;

    esym = elfsym(sym);
    if (!esym)
        return;

    if (sym->a.visibility)
        esym->st_other = (esym->st_other & ~ELFW(ST_VISIBILITY)(-1))
            | sym->a.visibility;

    if (sym->type.t & (VT_STATIC | VT_INLINE))
        sym_bind = STB_LOCAL;
    else if (sym->a.weak)
        sym_bind = STB_WEAK;
    else
        sym_bind = STB_GLOBAL;
    old_sym_bind = ELFW(ST_BIND)(esym->st_info);
    if (sym_bind != old_sym_bind) {
        esym->st_info = ELFW(ST_INFO)(sym_bind, ELFW(ST_TYPE)(esym->st_info));
    }
}

/* ------------------------------------------------------------------------- */
/* update sym->c so that it points to an external symbol in section
   'section' with value 'value' */

ST_FUNC void put_extern_sym2(Sym *sym, int sh_num,
                            addr_t value, unsigned long size)
{
    int sym_type, sym_bind, info, other, t;
    ElfSym *esym;
    const char *name;

    if (!sym->c) {
        name = get_tok_str(sym->v, NULL);
        t = sym->type.t;
        if ((t & VT_BTYPE) == VT_FUNC) {
            sym_type = STT_FUNC;
        } else if ((t & VT_BTYPE) == VT_VOID) {
            sym_type = STT_NOTYPE;
        } else {
            sym_type = STT_OBJECT;
        }
        if (t & (VT_STATIC | VT_INLINE))
            sym_bind = STB_LOCAL;
        else
            sym_bind = STB_GLOBAL;
        other = 0;


        if (sym->asm_label) {
            name = get_tok_str(sym->asm_label & ~SYM_FIELD, NULL);
            /* with SYM_FIELD it was __attribute__((alias("..."))) actually */
        }

        info = ELFW(ST_INFO)(sym_bind, sym_type);
        sym->c = put_elf_sym(symtab_section, value, size, info, other, sh_num, name);


    } else {
        esym = elfsym(sym);
        esym->st_value = value;
        esym->st_size = size;
        esym->st_shndx = sh_num;
    }
    update_storage(sym);
}

ST_FUNC void put_extern_sym(Sym *sym, Section *section,
                           addr_t value, unsigned long size)
{
    int sh_num = section ? section->sh_num : SHN_UNDEF;
    put_extern_sym2(sym, sh_num, value, size);
}

/* add a new relocation entry to symbol 'sym' in section 's' */
ST_FUNC void greloca(Section *s, Sym *sym, unsigned long offset, int type,
                     addr_t addend)
{
    int c = 0;

    if (nocode_wanted && s == cur_text_section)
        return;

    if (sym) {
        if (0 == sym->c)
            put_extern_sym(sym, NULL, 0, 0);
        c = sym->c;
    }

    /* now we can add ELF relocation info */
    put_elf_reloca(symtab_section, s, offset, type, c, addend);
}

#if PTR_SIZE == 4
ST_FUNC void greloc(Section *s, Sym *sym, unsigned long offset, int type)
{
    greloca(s, sym, offset, type, 0);
}
#endif

/* ------------------------------------------------------------------------- */
/* symbol allocator */
static Sym *__sym_malloc(void)
{
    Sym *sym_pool, *sym, *last_sym;
    int i;

    sym_pool = tcc_malloc(SYM_POOL_NB * sizeof(Sym));
    dynarray_add(&sym_pools, &nb_sym_pools, sym_pool);

    last_sym = sym_free_first;
    sym = sym_pool;
    for(i = 0; i < SYM_POOL_NB; i++) {
        sym->next = last_sym;
        last_sym = sym;
        sym++;
    }
    sym_free_first = last_sym;
    return last_sym;
}

ST_FUNC Sym *sym_malloc(void)
{
    Sym *sym;
#ifndef SYM_DEBUG
    sym = sym_free_first;
    if (!sym)
        sym = __sym_malloc();
    sym_free_first = sym->next;
    return sym;
#else
    sym = tcc_malloc(sizeof(Sym));
    return sym;
#endif
}

ST_INLN void sym_free(Sym *sym)
{
#ifndef SYM_DEBUG
    sym->next = sym_free_first;
    sym_free_first = sym;
#else
    tcc_free(sym);
#endif
}

ST_FUNC void symbols_finish(void)
{
    dynarray_reset(&sym_pools, &nb_sym_pools);
    sym_free_first = NULL;
}

/* push, without hashing */
ST_FUNC Sym *sym_push2(Sym **ps, int v, int t, int c)
{
    Sym *s;

    s = sym_malloc();
    memset(s, 0, sizeof *s);
    s->v = v;
    s->type.t = t;
    s->c = c;
    /* add in stack */
    s->prev = *ps;
    *ps = s;
    return s;
}

/* find a symbol and return its associated structure. 's' is the top
   of the symbol stack */
ST_FUNC Sym *sym_find2(Sym *s, int v)
{
    while (s) {
        if (s->v == v)
            return s;
        else if (s->v == -1)
            return NULL;
        s = s->prev;
    }
    return NULL;
}

/* structure lookup */
ST_INLN Sym *struct_find(int v)
{
    v -= TOK_IDENT;
    if ((unsigned)v >= (unsigned)(tok_ident - TOK_IDENT))
        return NULL;
    return table_ident[v]->sym_struct;
}

/* find an identifier */
ST_INLN Sym *sym_find(int v)
{
    v -= TOK_IDENT;
    if ((unsigned)v >= (unsigned)(tok_ident - TOK_IDENT))
        return NULL;
    return table_ident[v]->sym_identifier;
}

static int sym_scope(Sym *s)
{
  if (IS_ENUM_VAL (s->type.t))
    return s->type.ref->sym_scope;
  else
    return s->sym_scope;
}

/* push a given symbol on the symbol stack */
ST_FUNC Sym *sym_push(int v, CType *type, int r, int c)
{
    Sym *s, **ps;
    TokenSym *ts;

    if (local_stack)
        ps = &local_stack;
    else
        ps = &global_stack;
    s = sym_push2(ps, v, type->t, c);
    s->type.ref = type->ref;
    s->r = r;
    /* don't record fields or anonymous symbols */
    /* XXX: simplify */
    if (!(v & SYM_FIELD) && (v & ~SYM_STRUCT) < SYM_FIRST_ANOM) {
        /* record symbol in token array */
        ts = table_ident[(v & ~SYM_STRUCT) - TOK_IDENT];
        if (v & SYM_STRUCT)
            ps = &ts->sym_struct;
        else
            ps = &ts->sym_identifier;
        s->prev_tok = *ps;
        *ps = s;
        s->sym_scope = local_scope;
        if (s->prev_tok && sym_scope(s->prev_tok) == s->sym_scope)
            tcc_error("redeclaration of '%s'",
                get_tok_str(v & ~SYM_STRUCT, NULL));
    }
    return s;
}

/* push a global identifier */
ST_FUNC Sym *global_identifier_push(int v, int t, int c)
{
    Sym *s, **ps;
    s = sym_push2(&global_stack, v, t, c);
    s->r = VT_CONST | VT_SYM;
    /* don't record anonymous symbol */
    if (v < SYM_FIRST_ANOM) {
        ps = &table_ident[v - TOK_IDENT]->sym_identifier;
        /* modify the top most local identifier, so that sym_identifier will
           point to 's' when popped; happens when called from inline asm */
        while (*ps != NULL && (*ps)->sym_scope)
            ps = &(*ps)->prev_tok;
        s->prev_tok = *ps;
        *ps = s;
    }
    return s;
}

ST_FUNC Sym *external_global_sym(int v, CType *type)
{
    Sym *s = sym_find(v);

    if (!s) {
        s = global_identifier_push(v, type->t | VT_EXTERN, 0);
        s->type.ref = type->ref;
    } else if (IS_ASM_SYM(s)) {
        s->type.t = type->t | (s->type.t & VT_EXTERN);
        s->type.ref = type->ref;
        update_storage(s);
    }
    return s;
}

ST_FUNC void merge_symattr(struct SymAttr *to, struct SymAttr *from)
{
    if (from->aligned && !to->aligned)
        to->aligned = from->aligned;
    to->packed |= from->packed;
    to->weak |= from->weak;
    if (from->visibility != STV_DEFAULT
        && (to->visibility == STV_DEFAULT || to->visibility > from->visibility))
        to->visibility = from->visibility;
}

ST_FUNC void merge_funcattr(struct FuncAttr *to, struct FuncAttr *from)
{
    if (from->func_call && !to->func_call)
        to->func_call = from->func_call;
    if (from->func_type && !to->func_type)
        to->func_type = from->func_type;
    if (from->func_args && !to->func_args)
        to->func_args = from->func_args;
    if (from->func_noreturn)
        to->func_noreturn = 1;
    if (from->func_ctor)
        to->func_ctor = 1;
    if (from->func_dtor)
        to->func_dtor = 1;
}

static void patch_type(Sym *sym, CType *type)
{
    if (!(type->t & VT_EXTERN) || IS_ENUM_VAL(sym->type.t)) {
        if (!(sym->type.t & VT_EXTERN))
            tcc_error("redefinition of '%s'", get_tok_str(sym->v, NULL));
        sym->type.t &= ~VT_EXTERN;
    }

    if (IS_ASM_SYM(sym)) {
        sym->type.t = type->t & (sym->type.t | ~VT_STATIC);
        sym->type.ref = type->ref;
    }

    if (!is_compatible_types(&sym->type, type)) {
        tcc_error("incompatible types for redefinition of '%s'",
                  get_tok_str(sym->v, NULL));
    } else if ((sym->type.t & VT_BTYPE) == VT_FUNC) {
        int static_proto = sym->type.t & VT_STATIC;

        if ((type->t & VT_STATIC) && !static_proto
            && !((type->t | sym->type.t) & VT_INLINE))
            tcc_warning("static storage ignored for redefinition of '%s'",
                        get_tok_str(sym->v, NULL));
        if ((type->t | sym->type.t) & VT_INLINE) {
            if (!((type->t ^ sym->type.t) & VT_INLINE)
                || ((type->t | sym->type.t) & VT_STATIC))
                static_proto |= VT_INLINE;
        }

        if (!(type->t & VT_EXTERN)) {
            struct FuncAttr f = sym->type.ref->f;
            sym->type.t = (type->t & ~(VT_STATIC | VT_INLINE)) | static_proto;
            sym->type.ref = type->ref;
            merge_funcattr(&sym->type.ref->f, &f);
        } else {
            sym->type.t &= ~VT_INLINE | static_proto;
        }
        if (sym->type.ref->f.func_type == FUNC_OLD
            && type->ref->f.func_type != FUNC_OLD)
            sym->type.ref = type->ref;
    } else {
        if ((sym->type.t & VT_ARRAY) && type->ref->c >= 0)
            sym->type.ref->c = type->ref->c;
        if ((type->t ^ sym->type.t) & VT_STATIC)
            tcc_warning("storage mismatch for redefinition of '%s'",
                        get_tok_str(sym->v, NULL));
    }
}

ST_FUNC void patch_storage(Sym *sym, AttributeDef *ad, CType *type)
{
    if (type)
        patch_type(sym, type);
    merge_symattr(&sym->a, &ad->a);
    if (ad->asm_label)
        sym->asm_label = ad->asm_label;
    update_storage(sym);
}

static Sym *sym_copy(Sym *source, Sym **stack)
{
    Sym *copy = sym_malloc();

    *copy = *source;
    copy->prev = *stack;
    *stack = copy;
    if (copy->v < SYM_FIRST_ANOM) {
        stack = &table_ident[copy->v - TOK_IDENT]->sym_identifier;
        copy->prev_tok = *stack;
        *stack = copy;
    }
    return copy;
}

static void sym_copy_ref(Sym *sym, Sym **stack)
{
    int bt = sym->type.t & VT_BTYPE;

    if (bt == VT_FUNC || bt == VT_PTR) {
        Sym **next = &sym->type.ref;
        for (sym = *next, *next = NULL; sym; sym = sym->next) {
            Sym *copy = sym_copy(sym, stack);
            next = &(*next = copy)->next;
            sym_copy_ref(copy, stack);
        }
    }
}

ST_FUNC Sym *external_sym(int v, CType *type, int r, AttributeDef *ad)
{
    Sym *sym = sym_find(v);

    while (sym && sym->sym_scope)
        sym = sym->prev_tok;
    if (!sym) {
        sym = global_identifier_push(v, type->t, 0);
        sym->r |= r;
        sym->a = ad->a;
        sym->asm_label = ad->asm_label;
        sym->type.ref = type->ref;
        if (local_stack)
            sym_copy_ref(sym, &global_stack);
    } else {
        patch_storage(sym, ad, type);
    }
    if (local_stack && (sym->type.t & VT_BTYPE) != VT_FUNC)
        sym = sym_copy(sym, &local_stack);
    return sym;
}

/* pop symbols until top reaches 'b'.  If KEEP is non-zero don't really
   pop them yet from the list, but do remove them from the token array.  */
ST_FUNC void sym_pop(Sym **ptop, Sym *b, int keep)
{
    Sym *s, *ss, **ps;
    TokenSym *ts;
    int v;

    s = *ptop;
    while(s != b) {
        ss = s->prev;
        v = s->v;
        /* remove symbol in token array */
        /* XXX: simplify */
        if (!(v & SYM_FIELD) && (v & ~SYM_STRUCT) < SYM_FIRST_ANOM) {
            ts = table_ident[(v & ~SYM_STRUCT) - TOK_IDENT];
            if (v & SYM_STRUCT)
                ps = &ts->sym_struct;
            else
                ps = &ts->sym_identifier;
            *ps = s->prev_tok;
        }
	if (!keep)
	    sym_free(s);
        s = ss;
    }
    if (!keep)
	*ptop = b;
}

/* ------------------------------------------------------------------------- */
