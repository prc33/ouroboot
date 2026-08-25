/* Adapted from Blosc/MiniCC's LGPL-2.1 WebAssembly backend:
   https://github.com/Blosc/minicc */
#ifdef TARGET_DEFS_ONLY

/* virtual register file for wasm backend */
#define NB_REGS         12

/* register classes: keep INT and FLOAT separate to match tccgen expectations */
#define RC_INT          0x0001
#define RC_FLOAT        0x0002
#define RC_I0           0x0004
#define RC_I1           0x0008
#define RC_F0           0x0010
#define RC_F1           0x0020
#define RC_I64          0x0040
#define RC_L0           0x0080

#define RC_IRET         RC_I0
#define RC_IRE2         RC_I1
#define RC_FRET         RC_F0
#define RC_FRE2         RC_F1

enum {
    TREG_I0 = 0,
    TREG_I1,
    TREG_I2,
    TREG_I3,
    TREG_L0,
    TREG_L1,
    TREG_L2,
    TREG_L3,
    TREG_F0,
    TREG_F1,
    TREG_F2,
    TREG_F3
};

#define REG_IRET TREG_I0
#define REG_IRE2 TREG_I1
#define REG_LRET TREG_L0
#define REG_FRET TREG_F0
#define REG_FRE2 TREG_F1

#define PTR_SIZE 4
#define LDOUBLE_SIZE 8
#define LDOUBLE_ALIGN 8
#define MAX_ALIGN 16

#define TCC_USING_DOUBLE_FOR_LDOUBLE 1
#define PROMOTE_RET
#define TCC_NATIVE_I64

#else /* !TARGET_DEFS_ONLY */

#define USING_GLOBALS
#include "tcc.h"
#include "wasm-backend.h"

ST_DATA const char * const target_machine_defs =
    "__wasm32__\0"
    "__wasm__\0"
    ;

ST_DATA const int reg_classes[NB_REGS] = {
    RC_INT | RC_I0,
    RC_INT | RC_I1,
    RC_INT,
    RC_INT,
    RC_I64 | RC_L0,
    RC_I64,
    RC_I64,
    RC_I64,
    RC_FLOAT | RC_F0,
    RC_FLOAT | RC_F1,
    RC_FLOAT,
    RC_FLOAT,
};

ST_DATA WasmFuncIR *tcc_wasm_funcs;
ST_DATA int tcc_wasm_nb_funcs;

static int tcc_wasm_cap_funcs;
static WasmFuncIR *wasm_cur_func;

typedef struct WasmPatch {
    WasmFuncIR *func;
    int op_index;
    int next;
} WasmPatch;

static WasmPatch *wasm_patches;
static int wasm_nb_patches;
static int wasm_cap_patches;

static int wasm_last_cmp_valid;
static int wasm_last_cmp_op;

static NORETURN void wasm_unimp(const char *feature)
{
    tcc_error("wasm32 backend: %s is not supported in the restricted backend", feature);
}

static int wasm_is_float_type(int t)
{
    int bt = t & VT_BTYPE;
    return bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE;
}

static void wasm_grow(void **pp, int *cap, int new_count, int elem_size)
{
    int n;
    if (new_count <= *cap)
        return;
    n = *cap ? *cap : 16;
    while (n < new_count)
        n = n * 2;
    *pp = tcc_realloc(*pp, (unsigned long)n * elem_size);
    *cap = n;
}

static int wasm_type_to_val(int t, int for_ret)
{
    int bt = t & VT_BTYPE;

    switch (bt) {
    case VT_VOID:
        return WASM_VAL_VOID;
    case VT_BOOL:
    case VT_BYTE:
    case VT_SHORT:
    case VT_INT:
    case VT_ENUM:
    case VT_PTR:
    case VT_FUNC:
        return WASM_VAL_I32;
    case VT_FLOAT:
        return WASM_VAL_F32;
    case VT_DOUBLE:
    case VT_LDOUBLE:
        return WASM_VAL_F64;
    case VT_STRUCT:
        if (for_ret)
            return WASM_VAL_VOID;
        wasm_unimp("struct parameter");
        break;
    case VT_LLONG:
        return WASM_VAL_I64;
    }
    wasm_unimp("type in wasm type mapping");
    return WASM_VAL_VOID;
}

static char *wasm_tok_strdup(int tok)
{
    const char *s;
    if (tok < TOK_IDENT)
        return NULL;
    s = get_tok_str(tok, NULL);
    return s ? tcc_strdup(s) : NULL;
}

ST_FUNC void tcc_wasm_reset(void)
{
    int i, j;
    for (i = 0; i < tcc_wasm_nb_funcs; ++i) {
        for (j = 0; j < tcc_wasm_funcs[i].nb_ops; ++j) {
            tcc_free(tcc_wasm_funcs[i].ops[j].sym_name);
            tcc_free(tcc_wasm_funcs[i].ops[j].call_name);
        }
        tcc_free(tcc_wasm_funcs[i].name);
        tcc_free(tcc_wasm_funcs[i].param_types);
        tcc_free(tcc_wasm_funcs[i].param_offsets);
        tcc_free(tcc_wasm_funcs[i].ops);
    }
    tcc_free(tcc_wasm_funcs);
    tcc_wasm_funcs = NULL;
    tcc_wasm_nb_funcs = 0;
    tcc_wasm_cap_funcs = 0;

    tcc_free(wasm_patches);
    wasm_patches = NULL;
    wasm_nb_patches = 0;
    wasm_cap_patches = 0;

    wasm_cur_func = NULL;
    wasm_last_cmp_valid = 0;
    wasm_last_cmp_op = 0;
}

static WasmFuncIR *wasm_new_func(Sym *sym)
{
    WasmFuncIR *f;
    wasm_grow((void **)&tcc_wasm_funcs, &tcc_wasm_cap_funcs,
              tcc_wasm_nb_funcs + 1, sizeof(WasmFuncIR));
    f = &tcc_wasm_funcs[tcc_wasm_nb_funcs++];
    memset(f, 0, sizeof(*f));
    f->sym_tok = sym ? sym->v : 0;
    f->ret_type = WASM_VAL_VOID;
    return f;
}

static void wasm_func_add_param(WasmFuncIR *f, int type, int offset)
{
    int n = f->nb_params + 1;
    f->param_types = tcc_realloc(f->param_types, n * sizeof(*f->param_types));
    f->param_offsets = tcc_realloc(f->param_offsets, n * sizeof(*f->param_offsets));
    f->param_types[f->nb_params] = (unsigned char)type;
    f->param_offsets[f->nb_params] = offset;
    f->nb_params = n;
}

/* raw byte output used for section size accounting and generic helpers */
ST_FUNC void o(unsigned int c)
{
    int ind1 = ind + 1;
    if (nocode_wanted)
        return;
    if (ind1 > cur_text_section->data_allocated)
        section_realloc(cur_text_section, ind1);
    cur_text_section->data[ind] = c & 0xff;
    ind = ind1;
}

static WasmOp *wasm_emit_op(int kind)
{
    WasmOp *op;
    if (nocode_wanted || !wasm_cur_func)
        return NULL;
    wasm_grow((void **)&wasm_cur_func->ops, &wasm_cur_func->cap_ops,
              wasm_cur_func->nb_ops + 1, sizeof(WasmOp));
    op = &wasm_cur_func->ops[wasm_cur_func->nb_ops++];
    memset(op, 0, sizeof(*op));
    op->kind = (unsigned char)kind;
    op->pc = ind;
    op->target_pc = -1;
    o(0);
    return op;
}

static int wasm_add_patch(int op_index, int next)
{
    WasmPatch *p;
    int idx;
    wasm_grow((void **)&wasm_patches, &wasm_cap_patches,
              wasm_nb_patches + 1, sizeof(WasmPatch));
    idx = wasm_nb_patches++;
    p = &wasm_patches[idx];
    p->func = wasm_cur_func;
    p->op_index = op_index;
    p->next = next;
    return idx + 1;
}

static int wasm_cmp_invert(int op)
{
    if (!wasm_last_cmp_valid) {
        tcc_error("wasm32 backend: conditional branch without prior comparison");
    }
    if (op == wasm_last_cmp_op)
        return 0;
    if (op == (wasm_last_cmp_op ^ 1))
        return 1;
    /* Mismatch: the requested op doesn't match the last emitted
       comparison.  This happens in ternary expressions where the TRUE
       and FALSE branches generate different comparison types.  In the
       switch-loop dispatch, only one branch executes at runtime, so
       local_cmp will hold the result from the matching branch's
       comparison — which matches the requested op.  No inversion. */
    return 0;
}

static void wasm_set_addr(WasmOp *op, int fr, Sym *sym, int fc)
{
    unsigned value_flags;
    if (!op)
        return;
    value_flags = op->flags & 0xff00;
    if (fr == VT_LOCAL) {
        op->flags = value_flags | WASM_ADDR_FP;
        op->imm = fc;
    } else if (fr == VT_CONST) {
        if (sym) {
            op->flags = value_flags | WASM_ADDR_SYM;
            op->sym_index = sym->c;
            op->sym_tok = sym->v;
            op->sym_name = wasm_tok_strdup(sym->v);
        } else {
            op->flags = value_flags | WASM_ADDR_ABS;
        }
        op->imm = fc;
    } else if (fr >= 0 && fr < VT_CONST) {
        op->flags = value_flags | WASM_ADDR_REG;
        op->r1 = fr;
        op->imm = fc;
    } else {
        tcc_error("wasm32 backend: unsupported address mode");
    }
}

static void wasm_emit_cmp_set_i32(int op, int lhs_reg, int rhs_reg, int rhs_imm, int is_imm)
{
    WasmOp *wo = wasm_emit_op(WASM_OP_SET_CMP_I32);
    if (!wo)
        return;
    wo->r0 = lhs_reg;
    wo->op = op;
    if (is_imm) {
        wo->flags |= WASM_OP_FLAG_IMM;
        wo->imm = rhs_imm;
    } else {
        wo->r1 = rhs_reg;
    }
    wasm_last_cmp_valid = 1;
    wasm_last_cmp_op = op;
}

static void wasm_emit_cmp_set_i64(int op, int lhs_reg, int rhs_reg,
                                  int64_t rhs_imm, int is_imm)
{
    WasmOp *wo = wasm_emit_op(WASM_OP_SET_CMP_I64);
    if (!wo)
        return;
    wo->op = op;
    wo->r0 = lhs_reg;
    if (is_imm) {
        wo->flags |= WASM_OP_FLAG_IMM;
        wo->i64 = rhs_imm;
    } else {
        wo->r1 = rhs_reg;
    }
    wasm_last_cmp_valid = 1;
    wasm_last_cmp_op = op;
}

ST_FUNC void gsym_addr(int t, int a)
{
    while (t) {
        WasmPatch *p;
        int next;
        if (t <= 0 || t > wasm_nb_patches)
            tcc_error("wasm32 backend: invalid jump patch list");
        p = &wasm_patches[t - 1];
        if (p->func != wasm_cur_func)
            tcc_error("wasm32 backend: cross-function patch list");
        next = p->next;
        p->func->ops[p->op_index].target_pc = a;
        t = next;
    }
}

ST_FUNC void load(int r, SValue *sv)
{
    int v, ft, fr, fc, bt;
    SValue v1;
    WasmOp *wo;

    fr = sv->r;
    ft = sv->type.t & ~VT_DEFSIGN;
    fc = sv->c.i;
    ft &= ~(VT_VOLATILE | VT_CONSTANT);
    v = fr & VT_VALMASK;
    bt = ft & VT_BTYPE;

    if (bt == VT_STRUCT)
        wasm_unimp("struct load");

    if (fr & VT_LVAL) {
        if (v == VT_LLOCAL) {
            int tr = r;
            v1.type.t = VT_INT;
            v1.type.ref = NULL;
            v1.r = VT_LOCAL | VT_LVAL;
            v1.c.i = fc;
            v1.sym = NULL;
            if (!(reg_classes[tr] & RC_INT))
                tr = get_reg(RC_INT);
            load(tr, &v1);
            fr = tr | VT_LVAL;
            v = tr;
            fc = 0;
        }

        if (bt == VT_FLOAT) wo = wasm_emit_op(WASM_OP_LOAD_F32);
        else if (bt == VT_DOUBLE || bt == VT_LDOUBLE) wo = wasm_emit_op(WASM_OP_LOAD_F64);
        else if (bt == VT_LLONG) wo = wasm_emit_op(WASM_OP_LOAD_I64);
        else if (reg_classes[r] & RC_I64) {
            wo = wasm_emit_op(WASM_OP_LOAD_I32_I64);
            if (wo && (ft & VT_UNSIGNED)) wo->flags |= WASM_OP_FLAG_UNSIGNED;
        }
        else if ((ft & VT_TYPE) == VT_BYTE || (ft & VT_TYPE) == VT_BOOL) wo = wasm_emit_op(WASM_OP_LOAD_S8);
        else if ((ft & VT_TYPE) == (VT_BYTE | VT_UNSIGNED) ||
                 (ft & VT_TYPE) == (VT_BOOL | VT_UNSIGNED)) wo = wasm_emit_op(WASM_OP_LOAD_U8);
        else if ((ft & VT_TYPE) == VT_SHORT) wo = wasm_emit_op(WASM_OP_LOAD_S16);
        else if ((ft & VT_TYPE) == (VT_SHORT | VT_UNSIGNED)) wo = wasm_emit_op(WASM_OP_LOAD_U16);
        else wo = wasm_emit_op(WASM_OP_LOAD_I32);

        if (!wo)
            return;
        wo->r0 = r;
        wasm_set_addr(wo, fr & VT_VALMASK, (fr & VT_SYM) ? sv->sym : NULL, fc);
    } else if (v == VT_CONST) {
        if (bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE) {
            if (fr & VT_SYM) {
                wo = wasm_emit_op(bt == VT_FLOAT ? WASM_OP_LOAD_F32 : WASM_OP_LOAD_F64);
                if (!wo)
                    return;
                wo->r0 = r;
                wo->flags = WASM_ADDR_SYM;
                wo->sym_index = sv->sym ? sv->sym->c : 0;
                wo->sym_tok = sv->sym ? sv->sym->v : 0;
                wo->sym_name = sv->sym ? wasm_tok_strdup(sv->sym->v) : NULL;
                wo->imm = fc;
                return;
            }
            wo = wasm_emit_op(WASM_OP_F64_CONST);
            if (!wo)
                return;
            wo->r0 = r;
            if (bt == VT_FLOAT)
                wo->f64 = sv->c.f;
            else
                wo->f64 = sv->c.d;
        } else {
            if (fr & VT_SYM) {
                wo = wasm_emit_op(WASM_OP_ADDR_SYM);
                if (!wo)
                    return;
                wo->r0 = r;
                wo->sym_index = sv->sym ? sv->sym->c : 0;
                wo->sym_tok = sv->sym ? sv->sym->v : 0;
                wo->sym_name = sv->sym ? wasm_tok_strdup(sv->sym->v) : NULL;
                wo->imm = fc;
                return;
            }
            wo = wasm_emit_op((bt == VT_LLONG || (reg_classes[r] & RC_I64))
                              ? WASM_OP_I64_CONST : WASM_OP_I32_CONST);
            if (!wo)
                return;
            wo->r0 = r;
            if (bt == VT_LLONG || (reg_classes[r] & RC_I64))
                wo->i64 = sv->c.i;
            else
                wo->imm = (int)sv->c.i;
        }
    } else if (v == VT_LOCAL) {
        wo = wasm_emit_op(WASM_OP_ADDR_LOCAL);
        if (!wo)
            return;
        wo->r0 = r;
        wo->imm = fc;
    } else if (v == VT_CMP) {
        int inv = 0;
        if (fc <= 1) {
            wo = wasm_emit_op(WASM_OP_I32_CONST);
            if (!wo)
                return;
            wo->r0 = r;
            wo->imm = !!fc;
            return;
        }
        inv = wasm_cmp_invert(fc);
        wo = wasm_emit_op(WASM_OP_SET_I32_FROM_CMP);
        if (!wo)
            return;
        wo->r0 = r;
        wo->flags = inv ? WASM_OP_FLAG_INVERT : 0;
    } else if (v == VT_JMP || v == VT_JMPI) {
        int t = v & 1;
        int j;

        wo = wasm_emit_op(WASM_OP_I32_CONST);
        if (wo) {
            wo->r0 = r;
            wo->imm = t;
        }
        j = gjmp(0);
        gsym(fc);
        wo = wasm_emit_op(WASM_OP_I32_CONST);
        if (wo) {
            wo->r0 = r;
            wo->imm = t ^ 1;
        }
        gsym(j);
    } else if (v != r) {
        wo = wasm_emit_op((reg_classes[r] & RC_FLOAT) ? WASM_OP_MOV_F64 :
                          (reg_classes[r] & RC_I64) ? WASM_OP_MOV_I64 : WASM_OP_MOV_I32);
        if (!wo)
            return;
        wo->r0 = r;
        wo->r1 = v;
    }
}

ST_FUNC void store(int r, SValue *v)
{
    int ft, bt, fr, fc;
    WasmOp *wo;

    ft = v->type.t;
    bt = ft & VT_BTYPE;
    fr = v->r & VT_VALMASK;
    fc = v->c.i;

    if (bt == VT_STRUCT)
        wasm_unimp("struct store");

    if (fr == VT_CONST || fr == VT_LOCAL || (v->r & VT_LVAL)) {
        if (bt == VT_FLOAT) wo = wasm_emit_op(WASM_OP_STORE_F32);
        else if (bt == VT_DOUBLE || bt == VT_LDOUBLE) wo = wasm_emit_op(WASM_OP_STORE_F64);
        else if (bt == VT_LLONG) wo = wasm_emit_op(WASM_OP_STORE_I64);
        else if (bt == VT_SHORT) wo = wasm_emit_op(WASM_OP_STORE_I16);
        else if (bt == VT_BYTE || bt == VT_BOOL) wo = wasm_emit_op(WASM_OP_STORE_I8);
        else wo = wasm_emit_op(WASM_OP_STORE_I32);

        if (!wo)
            return;
        wo->r0 = r;
        wasm_set_addr(wo, fr, (v->r & VT_SYM) ? v->sym : NULL, fc);
    } else if (fr != r) {
        wo = wasm_emit_op((reg_classes[fr] & RC_FLOAT) ? WASM_OP_MOV_F64 :
                          (reg_classes[fr] & RC_I64) ? WASM_OP_MOV_I64 : WASM_OP_MOV_I32);
        if (!wo)
            return;
        wo->r0 = fr;
        wo->r1 = r;
    }
}

ST_FUNC int gfunc_sret(CType *vt, int variadic, CType *ret, int *ret_align, int *regsize)
{
    (void)variadic;
    (void)ret;
    *ret_align = 4;
    *regsize = 4;
    if ((vt->t & VT_BTYPE) == VT_STRUCT)
        return 0;
    return 1;
}

ST_FUNC void gfunc_call(int nb_args)
{
    Sym *func_sym;
    WasmOp *wo, *sop;
    int i, r, r2;
    int arg_types[WASM_MAX_CALL_ARGS];
    int arg_off[WASM_MAX_CALL_ARGS];
    int ret_type;
    int is_direct;
    int func_reg;

    if (nb_args > WASM_MAX_CALL_ARGS)
        tcc_error("wasm32 backend: too many call arguments (%d)", nb_args);

    save_regs(nb_args + 1);

    for (i = nb_args - 1; i >= 0; --i) {
        int bt = vtop->type.t & VT_BTYPE;
        int size, align, slot;
        r2 = VT_CONST;
        if (bt == VT_STRUCT)
            wasm_unimp("struct argument passing");
        if (wasm_is_float_type(vtop->type.t)) {
            r = gv(RC_FLOAT);
            arg_types[i] = wasm_type_to_val(vtop->type.t, 0);
        } else if (bt == VT_LLONG) {
            r = gv(RC_I64);
            arg_types[i] = WASM_VAL_I64;
        } else {
            r = gv(RC_INT);
            arg_types[i] = WASM_VAL_I32;
        }
        if (arg_types[i] == WASM_VAL_I64)
            size = 8, align = 8;
        else if (arg_types[i] == WASM_VAL_F64)
            size = 8, align = 8;
        else
            size = 4, align = 4;
        slot = (loc - size) & -align;
        loc = slot;
        if (arg_types[i] == WASM_VAL_I64)
            sop = wasm_emit_op(WASM_OP_STORE_I64);
        else if (arg_types[i] == WASM_VAL_F32)
            sop = wasm_emit_op(WASM_OP_STORE_F32);
        else if (arg_types[i] == WASM_VAL_F64)
            sop = wasm_emit_op(WASM_OP_STORE_F64);
        else
            sop = wasm_emit_op(WASM_OP_STORE_I32);
        if (sop) {
            sop->r0 = r;
            sop->r2 = VT_CONST;
            sop->flags = WASM_ADDR_FP;
            sop->imm = slot;
        }
        arg_off[i] = slot;
        vtop--;
    }

    func_sym = vtop->type.ref;
    if (!func_sym)
        tcc_error("wasm32 backend: invalid function call target");
    if (func_sym->f.func_type == FUNC_ELLIPSIS)
        wasm_unimp("variadic call");

    is_direct = ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_CONST | VT_SYM)
                 && vtop->c.i == 0);
    if (!is_direct)
        wasm_unimp("indirect function call");
    wo = wasm_emit_op(WASM_OP_CALL);
    func_reg = VT_CONST;
    if (wo) {
        int bt = func_sym->type.t & VT_BTYPE;
        wo->call_tok = vtop->sym ? vtop->sym->v : func_sym->v;
        wo->call_name = wasm_tok_strdup(wo->call_tok);
        wo->r0 = func_reg;
        wo->call_nb_args = nb_args;
        for (i = 0; i < nb_args; ++i) {
            wo->call_arg_type[i] = (unsigned char)arg_types[i];
            wo->call_arg_off[i] = arg_off[i];
        }
        if (bt == VT_STRUCT)
            ret_type = WASM_VAL_VOID;
        else
            ret_type = wasm_type_to_val(func_sym->type.t, 1);
        wo->type = (unsigned char)ret_type;
    }

    wasm_last_cmp_valid = 0;
    vtop--;
}

ST_FUNC void gfunc_prolog(Sym *func_sym)
{
    CType *func_type = &func_sym->type;
    Sym *sym;

    if (!func_type->ref)
        tcc_error("wasm32 backend: malformed function type");

    sym = func_type->ref;
    if (sym->f.func_type == FUNC_ELLIPSIS)
        wasm_unimp("variadic function definition");
    if (sym->f.func_call != FUNC_CDECL)
        wasm_unimp("non-cdecl calling convention");

    wasm_cur_func = wasm_new_func(func_sym);
    wasm_cur_func->is_static = !!(func_sym->type.t & VT_STATIC);
    wasm_cur_func->name = tcc_strdup(get_tok_str(func_sym->v, NULL));
    wasm_cur_func->start_pc = ind;

    loc = 0;
    func_vc = 0;

    if ((func_vt.t & VT_BTYPE) == VT_STRUCT) {
        loc -= PTR_SIZE;
        func_vc = loc;
        wasm_cur_func->has_sret = 1;
        wasm_cur_func->sret_param_offset = loc;
        wasm_func_add_param(wasm_cur_func, WASM_VAL_I32, loc);
        wasm_cur_func->ret_type = WASM_VAL_VOID;
    } else {
        wasm_cur_func->ret_type = (unsigned char)wasm_type_to_val(func_vt.t, 1);
    }

    while ((sym = sym->next) != NULL) {
        CType *type = &sym->type;
        int size, align;
        int val_type;

        if ((type->t & VT_BTYPE) == VT_STRUCT)
            wasm_unimp("struct parameters");

        size = type_size(type, &align);
        if (align < 1)
            align = 1;
        size = (size + 3) & ~3;
        loc = (loc - size) & -align;
        sym_push(sym->v & ~SYM_FIELD, &sym->type, VT_LOCAL | VT_LVAL, loc);

        val_type = wasm_type_to_val(type->t, 0);
        wasm_func_add_param(wasm_cur_func, val_type, loc);
    }

    wasm_last_cmp_valid = 0;
}

ST_FUNC void gfunc_epilog(void)
{
    WasmOp *wo;

    if (!wasm_cur_func)
        return;

    wasm_cur_func->frame_size = (-loc + 15) & -16;

    wo = wasm_emit_op(WASM_OP_RET);
    if (wo)
        wo->type = wasm_cur_func->ret_type;

    wasm_cur_func->end_pc = ind;
    wasm_cur_func = NULL;
    wasm_last_cmp_valid = 0;
}

ST_FUNC void gen_fill_nops(int bytes)
{
    while (bytes-- > 0)
        o(0);
}

ST_FUNC int gjmp(int t)
{
    WasmOp *wo;
    if (nocode_wanted)
        return t;
    wo = wasm_emit_op(WASM_OP_JMP);
    if (!wo)
        return t;
    return wasm_add_patch(wasm_cur_func->nb_ops - 1, t);
}

ST_FUNC void gjmp_addr(int a)
{
    WasmOp *wo;
    if (nocode_wanted)
        return;
    wo = wasm_emit_op(WASM_OP_JMP);
    if (!wo)
        return;
    wo->target_pc = a;
}

ST_FUNC int gjmp_cond(int op, int t)
{
    WasmOp *wo;
    int inv;
    if (nocode_wanted)
        return t;
    inv = wasm_cmp_invert(op);
    wo = wasm_emit_op(WASM_OP_JMP_CMP);
    if (!wo)
        return t;
    if (inv)
        wo->flags |= WASM_OP_FLAG_INVERT;
    return wasm_add_patch(wasm_cur_func->nb_ops - 1, t);
}

ST_FUNC int gjmp_append(int n, int t)
{
    if (n) {
        int tail = n;
        while (wasm_patches[tail - 1].next)
            tail = wasm_patches[tail - 1].next;
        wasm_patches[tail - 1].next = t;
        return n;
    }
    return t;
}

static void wasm_emit_i32_bin(int op, int dst, int src_reg, int src_imm, int is_imm)
{
    WasmOp *wo = wasm_emit_op(WASM_OP_I32_BIN);
    if (!wo)
        return;
    wo->r0 = dst;
    wo->op = op;
    if (is_imm) {
        wo->flags |= WASM_OP_FLAG_IMM;
        wo->imm = src_imm;
    } else {
        wo->r1 = src_reg;
    }
}

ST_FUNC void gen_opi(int op)
{
    int r, fr, c;
    WasmOp *wo;

    switch (op) {
    case TOK_MID:
        wo = wasm_emit_op(WASM_OP_I32_NEG);
        r = gv(RC_INT);
        if (wo)
            wo->r0 = r;
        break;

    case '+':
    case '-':
    case '&':
    case '^':
    case '|':
    case '*':
    case TOK_SHL:
    case TOK_SHR:
    case TOK_SAR:
    case '/':
    case TOK_UDIV:
    case TOK_PDIV:
    case '%':
    case TOK_UMOD:
        if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
            vswap();
            r = gv(RC_INT);
            vswap();
            c = vtop->c.i;
            wasm_emit_i32_bin(op, r, 0, c, 1);
        } else {
            gv2(RC_INT, RC_INT);
            r = vtop[-1].r;
            fr = vtop[0].r;
            wasm_emit_i32_bin(op, r, fr, 0, 0);
        }
        wasm_last_cmp_valid = 0;
        vtop--;
        break;

    case TOK_EQ:
    case TOK_NE:
    case TOK_LT:
    case TOK_GT:
    case TOK_LE:
    case TOK_GE:
    case TOK_ULT:
    case TOK_UGT:
    case TOK_ULE:
    case TOK_UGE:
        if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
            vswap();
            r = gv(RC_INT);
            vswap();
            c = vtop->c.i;
            wasm_emit_cmp_set_i32(op, r, 0, c, 1);
        } else {
            gv2(RC_INT, RC_INT);
            r = vtop[-1].r;
            fr = vtop[0].r;
            wasm_emit_cmp_set_i32(op, r, fr, 0, 0);
        }
        vtop--;
        vset_VT_CMP(op);
        break;

    default:
        tcc_error("wasm32 backend: unsupported integer op token %d", op);
        break;
    }
}

ST_FUNC void gen_opl(int op)
{
    int r, fr;
    int64_t c;
    WasmOp *wo;

    if (op == TOK_MID) {
        r = gv(RC_I64);
        wo = wasm_emit_op(WASM_OP_I64_NEG);
        if (wo) wo->r0 = r;
        return;
    }

    switch (op) {
    case '+': case '-': case '&': case '^': case '|': case '*':
    case TOK_SHL: case TOK_SHR: case TOK_SAR:
    case '/': case TOK_UDIV: case TOK_PDIV: case '%': case TOK_UMOD:
        if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
            vswap(); r = gv(RC_I64); vswap();
            c = vtop->c.i;
            wo = wasm_emit_op(WASM_OP_I64_BIN);
            if (wo) {
                wo->op = op; wo->r0 = r;
                wo->flags = WASM_OP_FLAG_IMM; wo->i64 = c;
            }
        } else {
            gv2(RC_I64, RC_I64);
            r = vtop[-1].r; fr = vtop[0].r;
            wo = wasm_emit_op(WASM_OP_I64_BIN);
            if (wo) { wo->op = op; wo->r0 = r; wo->r1 = fr; }
        }
        wasm_last_cmp_valid = 0;
        vtop--;
        return;

    case TOK_EQ: case TOK_NE: case TOK_LT: case TOK_GT: case TOK_LE:
    case TOK_GE: case TOK_ULT: case TOK_UGT: case TOK_ULE: case TOK_UGE:
        if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
            vswap(); r = gv(RC_I64); vswap();
            wasm_emit_cmp_set_i64(op, r, 0, vtop->c.i, 1);
        } else {
            gv2(RC_I64, RC_I64);
            r = vtop[-1].r; fr = vtop[0].r;
            wasm_emit_cmp_set_i64(op, r, fr, 0, 0);
        }
        vtop--;
        vset_VT_CMP(op);
        return;
    }
    tcc_error("wasm32 backend: unsupported i64 op token %d", op);
}

ST_FUNC void gen_cvt_i32_i64(int is_unsigned)
{
    int src = gv(RC_INT);
    int dst = get_reg(RC_I64);
    WasmOp *wo = wasm_emit_op(WASM_OP_EXTEND_I32_I64);
    if (wo) { wo->r0 = dst; wo->r1 = src; wo->flags = is_unsigned; }
    vtop->r = dst;
}

ST_FUNC void gen_cvt_i64_i32(void)
{
    int src = gv(RC_I64);
    int dst = get_reg(RC_INT);
    WasmOp *wo = wasm_emit_op(WASM_OP_WRAP_I64_I32);
    if (wo) { wo->r0 = dst; wo->r1 = src; }
    vtop->r = dst;
}

ST_FUNC void gen_opf(int op)
{
    int r, fr, bt;
    WasmOp *wo;

    if (op == TOK_MID) {
        r = gv(RC_FLOAT);
        bt = vtop->type.t & VT_BTYPE;
        wo = wasm_emit_op(bt == VT_FLOAT ? WASM_OP_F32_NEG : WASM_OP_F64_NEG);
        if (wo)
            wo->r0 = r;
        wasm_last_cmp_valid = 0;
        return;
    }

    gv2(RC_FLOAT, RC_FLOAT);
    r = vtop[-1].r;
    fr = vtop[0].r;
    bt = vtop[-1].type.t & VT_BTYPE;

    if (op >= TOK_ULT && op <= TOK_GT) {
        if (bt == VT_FLOAT)
            wo = wasm_emit_op(WASM_OP_SET_CMP_F32);
        else
            wo = wasm_emit_op(WASM_OP_SET_CMP_F64);
        if (wo) {
            wo->r0 = r;
            wo->r1 = fr;
            wo->op = op;
        }
        wasm_last_cmp_valid = 1;
        wasm_last_cmp_op = op;
        vtop--;
        vset_VT_CMP(op);
    } else {
        if (bt == VT_FLOAT)
            wo = wasm_emit_op(WASM_OP_F32_BIN);
        else
            wo = wasm_emit_op(WASM_OP_F64_BIN);
        if (wo) {
            wo->r0 = r;
            wo->r1 = fr;
            wo->op = op;
        }
        wasm_last_cmp_valid = 0;
        vtop--;
    }
}

ST_FUNC void gen_cvt_itof(int t)
{
    int r, fr, bt;
    WasmOp *wo;
    bt = vtop->type.t & VT_BTYPE;

    fr = get_reg(RC_FLOAT);
    if (bt == VT_LLONG) {
        r = gv(RC_I64);
        wo = wasm_emit_op((t & VT_BTYPE) == VT_FLOAT ? WASM_OP_I64_TOF_F32 : WASM_OP_I64_TOF_F64);
        if (wo) {
            wo->r0 = fr;
            wo->r1 = r;
            if (vtop->type.t & VT_UNSIGNED)
                wo->flags |= WASM_OP_FLAG_INVERT;
        }
    } else {
        r = gv(RC_INT);
        wo = wasm_emit_op((t & VT_BTYPE) == VT_FLOAT ? WASM_OP_ITOF_F32 : WASM_OP_ITOF_F64);
        if (wo) {
            wo->r0 = fr;
            wo->r1 = r;
            if (vtop->type.t & VT_UNSIGNED)
                wo->flags |= WASM_OP_FLAG_INVERT;
        }
    }

    vtop->r = fr;
    vtop->r2 = VT_CONST;
    wasm_last_cmp_valid = 0;
}

ST_FUNC void gen_cvt_ftoi(int t)
{
    int r, fr;
    WasmOp *wo;
    int bt = t & VT_BTYPE;

    fr = gv(RC_FLOAT);
    if (bt == VT_LLONG) {
        r = get_reg(RC_I64);
        wo = wasm_emit_op(WASM_OP_FTOI_I64);
        if (wo) {
            wo->r0 = r;
            wo->r1 = fr;
            if (t & VT_UNSIGNED)
                wo->flags |= WASM_OP_FLAG_INVERT;
        }
        vtop->r2 = VT_CONST;
    } else {
        r = get_reg(RC_INT);
        wo = wasm_emit_op(WASM_OP_FTOI_I32);
        if (wo) {
            wo->r0 = r;
            wo->r1 = fr;
            if (t & VT_UNSIGNED)
                wo->flags |= WASM_OP_FLAG_INVERT;
        }
        vtop->r2 = VT_CONST;
    }

    vtop->r = r;
    wasm_last_cmp_valid = 0;
}

ST_FUNC void gen_cvt_ftof(int t)
{
    WasmOp *wo;

    gv(RC_FLOAT);
    if ((t & VT_BTYPE) == VT_FLOAT) {
        wo = wasm_emit_op(WASM_OP_FTOF_TO_F32);
        if (wo)
            wo->r0 = vtop->r;
    }
    wasm_last_cmp_valid = 0;
}

ST_FUNC void ggoto(void)
{
    wasm_unimp("computed goto / indirect branch");
}

ST_FUNC void gen_vla_sp_save(int addr)
{
    (void)addr;
    wasm_unimp("VLA/alloca");
}

ST_FUNC void gen_vla_sp_restore(int addr)
{
    (void)addr;
    wasm_unimp("VLA/alloca");
}

ST_FUNC void gen_vla_alloc(CType *type, int align)
{
    (void)type;
    (void)align;
    wasm_unimp("VLA/alloca");
}

#endif /* !TARGET_DEFS_ONLY */
