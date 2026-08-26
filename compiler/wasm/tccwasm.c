/* Adapted from Blosc/MiniCC's LGPL-2.1 WebAssembly backend.
 * https://github.com/Blosc/minicc
 *
 *  wasm module output for TCC (restricted backend)
 */

#define USING_GLOBALS
#include "../tcc.h"
#include "wasm-backend.h"

typedef struct WasmBuf {
    unsigned char *data;
    int len;
    int cap;
} WasmBuf;

typedef struct WasmMemLayout {
    int rodata_base;
    int data_base;
    int bss_base;
    int stack_top;
} WasmMemLayout;

static WasmMemLayout wasm_layout;
static Section *wasm_sec_text;
static Section *wasm_sec_data;
static Section *wasm_sec_rodata;
static Section *wasm_sec_bss;

static int wasm_align_up(int v, int a)
{
    return (v + a - 1) & -a;
}

static void wb_reserve(WasmBuf *b, int add)
{
    int need = b->len + add;
    int n;
    if (need <= b->cap)
        return;
    n = b->cap ? b->cap : 256;
    while (n < need)
        n = n * 2;
    b->data = tcc_realloc(b->data, n);
    b->cap = n;
}

static void wb_u8(WasmBuf *b, int v)
{
    wb_reserve(b, 1);
    b->data[b->len++] = (unsigned char)v;
}

static void wb_mem(WasmBuf *b, const void *p, int n)
{
    wb_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void wb_uleb(WasmBuf *b, unsigned v)
{
    do {
        unsigned c = v & 0x7f;
        v >>= 7;
        if (v)
            c |= 0x80;
        wb_u8(b, c);
    } while (v);
}

static void wb_sleb(WasmBuf *b, int v)
{
    int more = 1;
    while (more) {
        int c = v & 0x7f;
        int sign = c & 0x40;
        v >>= 7;
        more = !((v == 0 && !sign) || (v == -1 && sign));
        if (more)
            c |= 0x80;
        wb_u8(b, c);
    }
}

static void wb_sleb64(WasmBuf *b, int64_t v)
{
    int more = 1;
    while (more) {
        int c = (int)(v & 0x7f);
        int sign = c & 0x40;
        v >>= 7;
        more = !((v == 0 && !sign) || (v == -1 && sign));
        if (more)
            c |= 0x80;
        wb_u8(b, c);
    }
}

static void wb_f64(WasmBuf *b, double x)
{
    union {
        double d;
        unsigned char c[8];
    } u;
    u.d = x;
    wb_mem(b, u.c, 8);
}

static int wasm_valtype_byte(int t)
{
    switch (t) {
    case WASM_VAL_I32: return 0x7f;
    case WASM_VAL_I64: return 0x7e;
    case WASM_VAL_F32: return 0x7d;
    case WASM_VAL_F64: return 0x7c;
    default:
        tcc_error("wasm32 backend: invalid wasm value type %d", t);
        return 0x40;
    }
}

static void wb_local_get(WasmBuf *b, int idx) { wb_u8(b, 0x20), wb_uleb(b, idx); }
static void wb_local_set(WasmBuf *b, int idx) { wb_u8(b, 0x21), wb_uleb(b, idx); }
static void wb_local_tee(WasmBuf *b, int idx) { wb_u8(b, 0x22), wb_uleb(b, idx); }
static void wb_global_get(WasmBuf *b, int idx) { wb_u8(b, 0x23), wb_uleb(b, idx); }
static void wb_global_set(WasmBuf *b, int idx) { wb_u8(b, 0x24), wb_uleb(b, idx); }
static void wb_i32_const(WasmBuf *b, int v) { wb_u8(b, 0x41), wb_sleb(b, v); }
static void wb_i64_const(WasmBuf *b, int64_t v) { wb_u8(b, 0x42), wb_sleb64(b, v); }
static void wb_f64_const(WasmBuf *b, double v) { wb_u8(b, 0x44), wb_f64(b, v); }

static void wb_memarg(WasmBuf *b, int align_log2)
{
    wb_uleb(b, align_log2);
    wb_uleb(b, 0);
}

static int wasm_i32_reg_local(int reg, int local_i0)
{
    if (reg < 0 || reg > 3)
        tcc_error("wasm32 backend: invalid integer register %d", reg);
    return local_i0 + reg;
}

static int wasm_f64_reg_local(int reg, int local_f0)
{
    if (reg < 8 || reg > 11)
        tcc_error("wasm32 backend: invalid floating register %d", reg);
    return local_f0 + (reg - 8);
}

static int wasm_i64_reg_local(int reg, int local_tmp64)
{
    if (reg < 4 || reg > 7)
        tcc_error("wasm32 backend: invalid i64 register %d", reg);
    return local_tmp64 - 4 + (reg - 4);
}

static int wasm_i32_bin_opcode(int op)
{
    switch (op) {
    case '+': return 0x6a;
    case '-': return 0x6b;
    case '*': return 0x6c;
    case '/': return 0x6d;
    case TOK_UDIV:
    case TOK_PDIV: return 0x6e;
    case '%': return 0x6f;
    case TOK_UMOD: return 0x70;
    case '&': return 0x71;
    case '|': return 0x72;
    case '^': return 0x73;
    case TOK_SHL: return 0x74;
    case TOK_SAR: return 0x75;
    case TOK_SHR: return 0x76;
    }
    tcc_error("wasm32 backend: unsupported i32 binop token %d", op);
    return 0x6a;
}

static int wasm_i32_cmp_opcode(int op)
{
    switch (op) {
    case TOK_EQ: return 0x46;
    case TOK_NE: return 0x47;
    case TOK_LT: return 0x48;
    case TOK_ULT: return 0x49;
    case TOK_GT: return 0x4a;
    case TOK_UGT: return 0x4b;
    case TOK_LE: return 0x4c;
    case TOK_ULE: return 0x4d;
    case TOK_GE: return 0x4e;
    case TOK_UGE: return 0x4f;
    }
    tcc_error("wasm32 backend: unsupported i32 cmp token %d", op);
    return 0x46;
}

static int wasm_i64_bin_opcode(int op)
{
    switch (op) {
    case '+': return 0x7c; case '-': return 0x7d; case '*': return 0x7e;
    case '/': return 0x7f; case TOK_UDIV: case TOK_PDIV: return 0x80;
    case '%': return 0x81; case TOK_UMOD: return 0x82;
    case '&': return 0x83; case '|': return 0x84; case '^': return 0x85;
    case TOK_SHL: return 0x86; case TOK_SAR: return 0x87; case TOK_SHR: return 0x88;
    }
    tcc_error("wasm32 backend: unsupported i64 binop token %d", op);
    return 0x7c;
}

static int wasm_i64_cmp_opcode(int op)
{
    switch (op) {
    case TOK_EQ: return 0x51; case TOK_NE: return 0x52;
    case TOK_LT: return 0x53; case TOK_ULT: return 0x54;
    case TOK_GT: return 0x55; case TOK_UGT: return 0x56;
    case TOK_LE: return 0x57; case TOK_ULE: return 0x58;
    case TOK_GE: return 0x59; case TOK_UGE: return 0x5a;
    }
    tcc_error("wasm32 backend: unsupported i64 comparison token %d", op);
    return 0x51;
}

static int wasm_f32_cmp_opcode(int op)
{
    switch (op) {
    case TOK_EQ: return 0x5b;
    case TOK_NE: return 0x5c;
    case TOK_LT:
    case TOK_ULT: return 0x5d;
    case TOK_GT:
    case TOK_UGT: return 0x5e;
    case TOK_LE:
    case TOK_ULE: return 0x5f;
    case TOK_GE:
    case TOK_UGE: return 0x60;
    }
    tcc_error("wasm32 backend: unsupported f32 cmp token %d", op);
    return 0x5b;
}

static int wasm_f64_cmp_opcode(int op)
{
    switch (op) {
    case TOK_EQ: return 0x61;
    case TOK_NE: return 0x62;
    case TOK_LT:
    case TOK_ULT: return 0x63;
    case TOK_GT:
    case TOK_UGT: return 0x64;
    case TOK_LE:
    case TOK_ULE: return 0x65;
    case TOK_GE:
    case TOK_UGE: return 0x66;
    }
    tcc_error("wasm32 backend: unsupported f64 cmp token %d", op);
    return 0x61;
}

static int wasm_f_bin_opcode(int op, int is_f32)
{
    switch (op) {
    case '+': return is_f32 ? 0x92 : 0xa0;
    case '-': return is_f32 ? 0x93 : 0xa1;
    case '*': return is_f32 ? 0x94 : 0xa2;
    case '/': return is_f32 ? 0x95 : 0xa3;
    }
    tcc_error("wasm32 backend: unsupported floating binop token %d", op);
    return is_f32 ? 0x92 : 0xa0;
}

static int wasm_find_func_index_by_tok(int tok)
{
    int i;
    for (i = 0; i < tcc_wasm_nb_funcs; ++i) {
        if (tcc_wasm_funcs[i].sym_tok == tok)
            return i;
    }
    return -1;
}

static int wasm_find_defined_sym_index_by_name(const char *name)
{
    ElfSym *symtab, *es;
    Section *strsec;
    int i, n, best = -1;

    if (!name || !*name || !symtab_section || !symtab_section->data)
        return -1;

    strsec = symtab_section->link;
    if (!strsec || !strsec->data)
        return -1;

    symtab = (ElfSym *)symtab_section->data;
    n = symtab_section->data_offset / sizeof(ElfSym);
    for (i = 1; i < n; ++i) {
        const char *sname;
        es = &symtab[i];
        if (es->st_shndx == SHN_UNDEF || es->st_name >= strsec->data_offset)
            continue;
        sname = (const char *)strsec->data + es->st_name;
        if (strcmp(sname, name))
            continue;
        if (ELFW(ST_TYPE)(es->st_info) != STT_SECTION)
            return i;
        if (best < 0)
            best = i;
    }
    return best;
}

static int wasm_func_ptr_value_from_sym_index(int sym_index, const char *name, int addend)
{
    (void)sym_index;
    (void)addend;
    tcc_error("wasm32 backend: function pointer '%s' is not supported",
              name ? name : "?");
    return 0;
}

static int wasm_sym_addr_from_elfsym(int sym_index, int addend)
{
    ElfSym *es;
    ElfSym *symtab;
    Section *strsec;
    const char *name = NULL;
    int sh;
    int base = 0;

    if (sym_index <= 0)
        return addend;
    if (!symtab_section || !symtab_section->data)
        tcc_error("wasm32 backend: missing symbol table for relocation");
    if ((unsigned)sym_index >= (unsigned)(symtab_section->data_offset / sizeof(ElfSym)))
        tcc_error("wasm32 backend: bad relocation symbol index %d", sym_index);

    symtab = (ElfSym *)symtab_section->data;
    es = &symtab[sym_index];
    sh = es->st_shndx;

    strsec = symtab_section->link;
    if (strsec && strsec->data && es->st_name < strsec->data_offset)
        name = (const char *)strsec->data + es->st_name;

    if (sh == SHN_UNDEF || (wasm_sec_text && sh == wasm_sec_text->sh_num))
        return wasm_func_ptr_value_from_sym_index(sym_index, name, addend);

    if (wasm_sec_rodata && sh == wasm_sec_rodata->sh_num)
        base = wasm_layout.rodata_base;
    else if (wasm_sec_data && sh == wasm_sec_data->sh_num)
        base = wasm_layout.data_base;
    else if (wasm_sec_bss && sh == wasm_sec_bss->sh_num)
        base = wasm_layout.bss_base;
    else if (sh == SHN_ABS)
        return (int)es->st_value + addend;
    else
        tcc_error("wasm32 backend: unsupported relocation section for symbol '%s'",
                  name ? name : "?");

    return base + (int)es->st_value + addend;
}

static int wasm_sym_addr_from_tok(int tok, const char *name, int addend)
{
    int si;

    if (tok < TOK_IDENT)
        return addend;
    if (!name || !*name)
        tcc_error("wasm32 backend: unresolved symbol token %d", tok);
    si = wasm_find_defined_sym_index_by_name(name);
    if (si > 0)
        return wasm_sym_addr_from_elfsym(si, addend);
    return wasm_func_ptr_value_from_sym_index(0, name, addend);
}

static int wasm_sym_addr_from_op(WasmOp *op)
{
    if (op->sym_index > 0)
        return wasm_sym_addr_from_elfsym(op->sym_index, op->imm);
    if (op->sym_tok >= TOK_IDENT)
        return wasm_sym_addr_from_tok(op->sym_tok, op->sym_name, op->imm);
    tcc_error("wasm32 backend: unresolved symbol reference in IR");
    return 0;
}

static void wasm_apply_data_relocs(Section *s)
{
    Section *sr;
    ElfW_Rel *rel;
    int i, n;

    if (!s || !s->data)
        return;
    sr = s->reloc;
    if (!sr || !sr->data)
        return;

    n = sr->data_offset / sizeof(ElfW_Rel);
    for (i = 0; i < n; ++i) {
        unsigned long off;
        unsigned char *ptr;
        int sym_index;
        int addend;
        int value;

        rel = (ElfW_Rel *)sr->data + i;
        off = rel->r_offset;
        if (off + 4 > (unsigned long)s->data_offset)
            tcc_error("wasm32 backend: relocation offset out of range in section '%s'", s->name);

        ptr = s->data + off;
        sym_index = ELFW(R_SYM)(rel->r_info);
#if SHT_RELX == SHT_RELA
        addend = rel->r_addend;
#else
        addend = (int)(int32_t)read32le(ptr);
#endif
        value = wasm_sym_addr_from_elfsym(sym_index, addend);
        write32le(ptr, value);
    }
}

static int wasm_validate_calls(void)
{
    int i, k;
    for (i = 0; i < tcc_wasm_nb_funcs; ++i) {
        WasmFuncIR *f = &tcc_wasm_funcs[i];
        for (k = 0; k < f->nb_ops; ++k) {
            WasmOp *op = &f->ops[k];
            if (op->kind != WASM_OP_CALL)
                continue;
            if (wasm_find_func_index_by_tok(op->call_tok) >= 0)
                continue;
            if (!op->call_name || !*op->call_name) {
                tcc_error_noabort("wasm32 backend: unresolved direct call token %d", op->call_tok);
                return -1;
            }
            tcc_error_noabort("wasm32 backend: undefined function '%s'", op->call_name);
            return -1;
        }
    }
    return 0;
}

static int wasm_pc_to_index(WasmFuncIR *f, int pc)
{
    int idx = pc - f->start_pc;
    if (pc == f->end_pc)
        return f->nb_ops;
    if (idx < 0 || idx >= f->nb_ops)
        tcc_error("wasm32 backend: bad jump target pc=%d (range %d..%d)",
                  pc, f->start_pc, f->end_pc);
    return idx;
}

static void wasm_emit_addr(WasmBuf *b, WasmOp *op, int local_fp, int local_i0)
{
    switch (op->flags & 0x00ff) {
    case WASM_ADDR_REG:
        wb_local_get(b, wasm_i32_reg_local(op->r1, local_i0));
        if (op->imm)
            wb_i32_const(b, op->imm), wb_u8(b, 0x6a);
        break;
    case WASM_ADDR_FP:
        wb_local_get(b, local_fp);
        if (op->imm)
            wb_i32_const(b, op->imm), wb_u8(b, 0x6a);
        break;
    case WASM_ADDR_ABS:
        wb_i32_const(b, op->imm);
        break;
    case WASM_ADDR_SYM:
        wb_i32_const(b, wasm_sym_addr_from_op(op));
        break;
    default:
        tcc_error("wasm32 backend: invalid address mode %u", op->flags & 0x00ff);
    }
}

static void wasm_emit_call_arg(WasmBuf *b, WasmOp *op, int i, int local_fp)
{
    int at = op->call_arg_type[i];
    wb_local_get(b, local_fp);
    if (op->call_arg_off[i])
        wb_i32_const(b, op->call_arg_off[i]), wb_u8(b, 0x6a);
    if (at == WASM_VAL_I32)
        wb_u8(b, 0x28), wb_memarg(b, 2);
    else if (at == WASM_VAL_I64)
        wb_u8(b, 0x29), wb_memarg(b, 3);
    else if (at == WASM_VAL_F32)
        wb_u8(b, 0x2a), wb_memarg(b, 2);
    else if (at == WASM_VAL_F64)
        wb_u8(b, 0x2b), wb_memarg(b, 3);
    else
        tcc_error("wasm32 backend: invalid call argument type %d", at);
}

/* Returns the wasm local index that op will read first, or -1 if unknown.
 * Used by peephole optimization to chain values on the wasm stack. */
static int wasm_op_first_input(WasmOp *op, int local_i0, int local_f0)
{
    switch (op->kind) {
    /* Ops that read r0 (dst) as first operand — integer */
    case WASM_OP_I32_BIN:
    case WASM_OP_SET_CMP_I32:
        if (op->r0 < 0 || op->r0 > 3) return -1;
        return wasm_i32_reg_local(op->r0, local_i0);

    /* Ops that read r0 (dst) as first operand — float */
    case WASM_OP_F64_BIN:
    case WASM_OP_F64_NEG:
    case WASM_OP_SET_CMP_F64:
    case WASM_OP_F32_BIN:
    case WASM_OP_F32_NEG:
    case WASM_OP_SET_CMP_F32:
    case WASM_OP_FTOF_TO_F32:
        if (op->r0 < 4 || op->r0 > 7) return -1;
        return wasm_f64_reg_local(op->r0, local_f0);

    /* Ops that read r1 as first operand (conversions: output to r0, input from r1) */
    case WASM_OP_ITOF_F32:
    case WASM_OP_ITOF_F64:
        if (op->r1 < 0 || op->r1 > 3) return -1;
        return wasm_i32_reg_local(op->r1, local_i0);
    case WASM_OP_FTOI_I32:
    case WASM_OP_FTOI_I64:
        if (op->r1 < 4 || op->r1 > 7) return -1;
        return wasm_f64_reg_local(op->r1, local_f0);

    /* MOV reads r1 */
    case WASM_OP_MOV_I32:
        if (op->r1 < 0 || op->r1 > 3) return -1;
        return wasm_i32_reg_local(op->r1, local_i0);
    case WASM_OP_MOV_F64:
        if (op->r1 < 4 || op->r1 > 7) return -1;
        return wasm_f64_reg_local(op->r1, local_f0);

    default:
        return -1;
    }
}

static void wasm_emit_case(WasmBuf *b, WasmFuncIR *f, WasmOp *op,
                           int case_index, int loop_depth, int cur_block,
                           int local_pc, int local_fp, int local_cmp, int local_carry,
                           int local_i0, int local_f0, int local_tmp64,
                           int *op_to_block, int nb_blocks, int emit_dispatch,
                           int stack_reg, int next_first_input, int *p_stack_out)
{
    int next_index = case_index + 1;
    int dst, src, target_index;

    *p_stack_out = -1;

    /* Peephole: emit local.tee (keep on stack) instead of local.set when
     * the next op in the same block will read this local first. */
#define WB_SET_OR_TEE(buf, local) do { \
    if ((local) == next_first_input && next_first_input >= 0) { \
        wb_local_tee(buf, local); \
        *p_stack_out = local; \
    } else { \
        wb_local_set(buf, local); \
    } \
} while (0)

    /* Peephole: skip local.get if the value is already on the wasm stack
     * from the previous op's local.tee. */
#define WB_GET_OR_SKIP(buf, local) do { \
    if ((local) == stack_reg && stack_reg >= 0) \
        stack_reg = -2; /* consumed */ \
    else \
        wb_local_get(buf, local); \
} while (0)

    if (next_index > f->nb_ops)
        next_index = f->nb_ops;

    switch (op->kind) {
    case WASM_OP_I32_CONST:
        dst = wasm_i32_reg_local(op->r0, local_i0);
        wb_i32_const(b, op->imm);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_I64_CONST:
        dst = wasm_i64_reg_local(op->r0, local_tmp64);
        wb_i64_const(b, op->i64);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_F64_CONST:
        dst = wasm_f64_reg_local(op->r0, local_f0);
        wb_f64_const(b, op->f64);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_MOV_I32:
        if (op->r0 >= 4) {
            dst = wasm_i64_reg_local(op->r0, local_tmp64);
            src = wasm_i32_reg_local(op->r1, local_i0);
            WB_GET_OR_SKIP(b, src);
            wb_u8(b, (op->flags & WASM_OP_FLAG_UNSIGNED) ? 0xad : 0xac);
            WB_SET_OR_TEE(b, dst);
            break;
        }
        dst = wasm_i32_reg_local(op->r0, local_i0);
        if (op->r1 >= 4) {
            src = wasm_i64_reg_local(op->r1, local_tmp64);
            WB_GET_OR_SKIP(b, src);
            wb_u8(b, 0xa7);
            WB_SET_OR_TEE(b, dst);
            break;
        }
        src = wasm_i32_reg_local(op->r1, local_i0);
        WB_GET_OR_SKIP(b, src);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_MOV_I64:
        if (op->r0 < 4) {
            dst = wasm_i32_reg_local(op->r0, local_i0);
            src = wasm_i64_reg_local(op->r1, local_tmp64);
            WB_GET_OR_SKIP(b, src);
            wb_u8(b, 0xa7); /* i32.wrap_i64 */
            WB_SET_OR_TEE(b, dst);
            break;
        }
        dst = wasm_i64_reg_local(op->r0, local_tmp64);
        if (op->r1 < 4) {
            src = wasm_i32_reg_local(op->r1, local_i0);
            WB_GET_OR_SKIP(b, src);
            wb_u8(b, (op->flags & WASM_OP_FLAG_UNSIGNED) ? 0xad : 0xac);
            WB_SET_OR_TEE(b, dst);
            break;
        }
        src = wasm_i64_reg_local(op->r1, local_tmp64);
        WB_GET_OR_SKIP(b, src);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_MOV_F64:
        dst = wasm_f64_reg_local(op->r0, local_f0);
        src = wasm_f64_reg_local(op->r1, local_f0);
        WB_GET_OR_SKIP(b, src);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_ADDR_LOCAL:
        dst = wasm_i32_reg_local(op->r0, local_i0);
        wb_local_get(b, local_fp);
        if (op->imm)
            wb_i32_const(b, op->imm), wb_u8(b, 0x6a);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_ADDR_SYM:
        dst = wasm_i32_reg_local(op->r0, local_i0);
        wb_i32_const(b, wasm_sym_addr_from_op(op));
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_LOAD_I32:
    case WASM_OP_LOAD_S8:
    case WASM_OP_LOAD_U8:
    case WASM_OP_LOAD_S16:
    case WASM_OP_LOAD_U16:
        dst = wasm_i32_reg_local(op->r0, local_i0);
        wasm_emit_addr(b, op, local_fp, local_i0);
        switch (op->kind) {
        case WASM_OP_LOAD_I32: wb_u8(b, 0x28), wb_memarg(b, 2); break;
        case WASM_OP_LOAD_S8: wb_u8(b, 0x2c), wb_memarg(b, 0); break;
        case WASM_OP_LOAD_U8: wb_u8(b, 0x2d), wb_memarg(b, 0); break;
        case WASM_OP_LOAD_S16: wb_u8(b, 0x2e), wb_memarg(b, 1); break;
        default: wb_u8(b, 0x2f), wb_memarg(b, 1); break;
        }
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_LOAD_I64:
        dst = wasm_i64_reg_local(op->r0, local_tmp64);
        wasm_emit_addr(b, op, local_fp, local_i0);
        wb_u8(b, 0x29), wb_memarg(b, 3);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_LOAD_I32_I64:
        dst = wasm_i64_reg_local(op->r0, local_tmp64);
        wasm_emit_addr(b, op, local_fp, local_i0);
        wb_u8(b, 0x28), wb_memarg(b, 2);
        wb_u8(b, (op->flags & WASM_OP_FLAG_UNSIGNED) ? 0xad : 0xac);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_LOAD_F32:
        dst = wasm_f64_reg_local(op->r0, local_f0);
        wasm_emit_addr(b, op, local_fp, local_i0);
        wb_u8(b, 0x2a), wb_memarg(b, 2);
        wb_u8(b, 0xbb); /* f64.promote_f32 */
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_LOAD_F64:
        dst = wasm_f64_reg_local(op->r0, local_f0);
        wasm_emit_addr(b, op, local_fp, local_i0);
        wb_u8(b, 0x2b), wb_memarg(b, 3);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_STORE_I32:
    case WASM_OP_STORE_I64:
    case WASM_OP_STORE_I8:
    case WASM_OP_STORE_I16:
        src = op->kind == WASM_OP_STORE_I64
            ? wasm_i64_reg_local(op->r0, local_tmp64)
            : wasm_i32_reg_local(op->r0, local_i0);
        wasm_emit_addr(b, op, local_fp, local_i0);
        if (op->kind == WASM_OP_STORE_I64) {
            wb_local_get(b, src);
            wb_u8(b, 0x37), wb_memarg(b, 3);
        } else {
            wb_local_get(b, src);
            if (op->kind == WASM_OP_STORE_I32)
                wb_u8(b, 0x36), wb_memarg(b, 2);
            else if (op->kind == WASM_OP_STORE_I8)
                wb_u8(b, 0x3a), wb_memarg(b, 0);
            else
                wb_u8(b, 0x3b), wb_memarg(b, 1);
        }
        break;

    case WASM_OP_STORE_F32:
        src = wasm_f64_reg_local(op->r0, local_f0);
        wasm_emit_addr(b, op, local_fp, local_i0);
        wb_local_get(b, src);
        wb_u8(b, 0xb6); /* f32.demote_f64 */
        wb_u8(b, 0x38), wb_memarg(b, 2);
        break;

    case WASM_OP_STORE_F64:
        src = wasm_f64_reg_local(op->r0, local_f0);
        wasm_emit_addr(b, op, local_fp, local_i0);
        wb_local_get(b, src);
        wb_u8(b, 0x39), wb_memarg(b, 3);
        break;

    case WASM_OP_I32_BIN:
        dst = wasm_i32_reg_local(op->r0, local_i0);
        WB_GET_OR_SKIP(b, dst);
        if (op->flags & WASM_OP_FLAG_IMM)
            wb_i32_const(b, op->imm);
        else
            wb_local_get(b, wasm_i32_reg_local(op->r1, local_i0));
        wb_u8(b, wasm_i32_bin_opcode(op->op));
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_I64_BIN:
        dst = wasm_i64_reg_local(op->r0, local_tmp64);
        WB_GET_OR_SKIP(b, dst);
        if (op->flags & WASM_OP_FLAG_IMM)
            wb_i64_const(b, op->i64);
        else
            wb_local_get(b, wasm_i64_reg_local(op->r1, local_tmp64));
        wb_u8(b, wasm_i64_bin_opcode(op->op));
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_I32_NEG:
        dst = wasm_i32_reg_local(op->r0, local_i0);
        wb_i32_const(b, 0);
        wb_local_get(b, dst);
        wb_u8(b, 0x6b);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_I64_NEG:
        dst = wasm_i64_reg_local(op->r0, local_tmp64);
        wb_i64_const(b, 0);
        wb_local_get(b, dst);
        wb_u8(b, 0x7d);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_F64_BIN:
        dst = wasm_f64_reg_local(op->r0, local_f0);
        src = wasm_f64_reg_local(op->r1, local_f0);
        WB_GET_OR_SKIP(b, dst);
        wb_local_get(b, src);
        wb_u8(b, wasm_f_bin_opcode(op->op, 0));
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_F32_BIN:
        dst = wasm_f64_reg_local(op->r0, local_f0);
        src = wasm_f64_reg_local(op->r1, local_f0);
        WB_GET_OR_SKIP(b, dst);
        wb_u8(b, 0xb6);
        wb_local_get(b, src), wb_u8(b, 0xb6);
        wb_u8(b, wasm_f_bin_opcode(op->op, 1));
        wb_u8(b, 0xbb);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_F64_NEG:
        dst = wasm_f64_reg_local(op->r0, local_f0);
        WB_GET_OR_SKIP(b, dst);
        wb_u8(b, 0x9a);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_F32_NEG:
        dst = wasm_f64_reg_local(op->r0, local_f0);
        WB_GET_OR_SKIP(b, dst);
        wb_u8(b, 0xb6);
        wb_u8(b, 0x8c);
        wb_u8(b, 0xbb);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_SET_CMP_I32:
    {
        int r0_local = wasm_i32_reg_local(op->r0, local_i0);
        WB_GET_OR_SKIP(b, r0_local);
        if (op->flags & WASM_OP_FLAG_IMM)
            wb_i32_const(b, op->imm);
        else
            wb_local_get(b, wasm_i32_reg_local(op->r1, local_i0));
        wb_u8(b, wasm_i32_cmp_opcode(op->op));
        wb_local_set(b, local_cmp);
        break;
    }

    case WASM_OP_SET_CMP_I64:
    {
        int r0_local = wasm_i64_reg_local(op->r0, local_tmp64);
        WB_GET_OR_SKIP(b, r0_local);
        if (op->flags & WASM_OP_FLAG_IMM)
            wb_i64_const(b, op->i64);
        else
            wb_local_get(b, wasm_i64_reg_local(op->r1, local_tmp64));
        wb_u8(b, wasm_i64_cmp_opcode(op->op));
        wb_local_set(b, local_cmp);
        break;
    }

    case WASM_OP_EXTEND_I32_I64:
        dst = wasm_i64_reg_local(op->r0, local_tmp64);
        wb_local_get(b, wasm_i32_reg_local(op->r1, local_i0));
        wb_u8(b, op->flags ? 0xad : 0xac);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_WRAP_I64_I32:
        dst = wasm_i32_reg_local(op->r0, local_i0);
        wb_local_get(b, wasm_i64_reg_local(op->r1, local_tmp64));
        wb_u8(b, 0xa7);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_SET_CMP_F32:
    {
        int r0_local = wasm_f64_reg_local(op->r0, local_f0);
        WB_GET_OR_SKIP(b, r0_local);
        wb_u8(b, 0xb6);
        wb_local_get(b, wasm_f64_reg_local(op->r1, local_f0)), wb_u8(b, 0xb6);
        wb_u8(b, wasm_f32_cmp_opcode(op->op));
        wb_local_set(b, local_cmp);
        break;
    }

    case WASM_OP_SET_CMP_F64:
    {
        int r0_local = wasm_f64_reg_local(op->r0, local_f0);
        WB_GET_OR_SKIP(b, r0_local);
        wb_local_get(b, wasm_f64_reg_local(op->r1, local_f0));
        wb_u8(b, wasm_f64_cmp_opcode(op->op));
        wb_local_set(b, local_cmp);
        break;
    }

    case WASM_OP_SET_I32_FROM_CMP:
        dst = wasm_i32_reg_local(op->r0, local_i0);
        wb_local_get(b, local_cmp);
        if (op->flags & WASM_OP_FLAG_INVERT)
            wb_u8(b, 0x45); /* i32.eqz */
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_JMP:
        target_index = wasm_pc_to_index(f, op->target_pc);
        if (op_to_block) {
            int bi = (target_index < f->nb_ops) ? op_to_block[target_index] : nb_blocks;
            if (bi < cur_block) {
                /* Forward jump (lower block index = later in code): direct br */
                wb_u8(b, 0x0c), wb_uleb(b, cur_block - bi - 1);
            } else if (bi == nb_blocks) {
                /* Jump to halt block: direct br */
                wb_u8(b, 0x0c), wb_uleb(b, loop_depth - 1);
            } else {
                /* Backward jump (higher block = earlier in code): dispatch */
                wb_i32_const(b, bi);
                wb_local_set(b, local_pc);
                wb_u8(b, 0x0c), wb_uleb(b, loop_depth);
            }
        } else {
            wb_i32_const(b, target_index);
            wb_local_set(b, local_pc);
            wb_u8(b, 0x0c), wb_uleb(b, loop_depth);
        }
        return;

    case WASM_OP_JMP_CMP:
        target_index = wasm_pc_to_index(f, op->target_pc);
        if (op_to_block) {
            int bi = (target_index < f->nb_ops) ? op_to_block[target_index] : nb_blocks;
            int next_bi = (next_index < f->nb_ops) ? op_to_block[next_index] : nb_blocks;
            /* "forward" means target < cur_block (lower block = later in execution)
             * or target == nb_blocks (halt block) */
            int target_fwd = (bi < cur_block) || (bi == nb_blocks);
            int next_fwd = (next_bi < cur_block) || (next_bi == nb_blocks);
            int target_depth = (bi == nb_blocks) ? loop_depth - 1 : cur_block - bi - 1;
            int next_depth = (next_bi == nb_blocks) ? loop_depth - 1 : cur_block - next_bi - 1;

            if (target_fwd && next_fwd) {
                /* Both forward: br_if + br */
                wb_local_get(b, local_cmp);
                if (op->flags & WASM_OP_FLAG_INVERT)
                    wb_u8(b, 0x45);
                wb_u8(b, 0x0d), wb_uleb(b, target_depth); /* br_if */
                wb_u8(b, 0x0c), wb_uleb(b, next_depth); /* br */
            } else if (target_fwd) {
                /* Target forward, fallthrough backward */
                wb_local_get(b, local_cmp);
                if (op->flags & WASM_OP_FLAG_INVERT)
                    wb_u8(b, 0x45);
                wb_u8(b, 0x0d), wb_uleb(b, target_depth); /* br_if */
                /* Fall through to dispatch for backward next */
                wb_i32_const(b, next_bi);
                wb_local_set(b, local_pc);
                wb_u8(b, 0x0c), wb_uleb(b, loop_depth);
            } else if (next_fwd) {
                /* Target backward, fallthrough forward: invert condition */
                wb_local_get(b, local_cmp);
                if (op->flags & WASM_OP_FLAG_INVERT)
                    wb_u8(b, 0x45);
                wb_u8(b, 0x45); /* i32.eqz — invert */
                wb_u8(b, 0x0d), wb_uleb(b, next_depth); /* br_if !cond */
                /* Fall through to dispatch for backward target */
                wb_i32_const(b, bi);
                wb_local_set(b, local_pc);
                wb_u8(b, 0x0c), wb_uleb(b, loop_depth);
            } else {
                /* Both backward: full dispatch */
                wb_local_get(b, local_cmp);
                if (op->flags & WASM_OP_FLAG_INVERT)
                    wb_u8(b, 0x45);
                wb_u8(b, 0x04), wb_u8(b, 0x7f); /* if (result i32) */
                wb_i32_const(b, bi);
                wb_u8(b, 0x05); /* else */
                wb_i32_const(b, next_bi);
                wb_u8(b, 0x0b); /* end */
                wb_local_set(b, local_pc);
                wb_u8(b, 0x0c), wb_uleb(b, loop_depth);
            }
        } else {
            wb_local_get(b, local_cmp);
            if (op->flags & WASM_OP_FLAG_INVERT)
                wb_u8(b, 0x45);
            wb_u8(b, 0x04), wb_u8(b, 0x7f); /* if (result i32) */
            wb_i32_const(b, target_index);
            wb_u8(b, 0x05); /* else */
            wb_i32_const(b, next_index);
            wb_u8(b, 0x0b); /* end */
            wb_local_set(b, local_pc);
            wb_u8(b, 0x0c), wb_uleb(b, loop_depth);
        }
        return;

    case WASM_OP_ITOF_F32:
        dst = wasm_f64_reg_local(op->r0, local_f0);
        {
            int r1_local = wasm_i32_reg_local(op->r1, local_i0);
            WB_GET_OR_SKIP(b, r1_local);
        }
        wb_u8(b, (op->flags & WASM_OP_FLAG_INVERT) ? 0xb3 : 0xb2);
        wb_u8(b, 0xbb);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_ITOF_F64:
        dst = wasm_f64_reg_local(op->r0, local_f0);
        {
            int r1_local = wasm_i32_reg_local(op->r1, local_i0);
            WB_GET_OR_SKIP(b, r1_local);
        }
        wb_u8(b, (op->flags & WASM_OP_FLAG_INVERT) ? 0xb8 : 0xb7);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_I64_TOF_F32:
        dst = wasm_f64_reg_local(op->r0, local_f0);
        wb_local_get(b, wasm_i64_reg_local(op->r1, local_tmp64));
        wb_u8(b, (op->flags & WASM_OP_FLAG_INVERT) ? 0xb5 : 0xb4);
        wb_u8(b, 0xbb);
        wb_local_set(b, dst);
        break;

    case WASM_OP_I64_TOF_F64:
        dst = wasm_f64_reg_local(op->r0, local_f0);
        wb_local_get(b, wasm_i64_reg_local(op->r1, local_tmp64));
        wb_u8(b, (op->flags & WASM_OP_FLAG_INVERT) ? 0xba : 0xb9);
        wb_local_set(b, dst);
        break;

    case WASM_OP_FTOI_I32:
        dst = wasm_i32_reg_local(op->r0, local_i0);
        {
            int r1_local = wasm_f64_reg_local(op->r1, local_f0);
            WB_GET_OR_SKIP(b, r1_local);
        }
        wb_u8(b, (op->flags & WASM_OP_FLAG_INVERT) ? 0xab : 0xaa);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_FTOI_I64:
        dst = wasm_i64_reg_local(op->r0, local_tmp64);
        {
            int r1_local = wasm_f64_reg_local(op->r1, local_f0);
            WB_GET_OR_SKIP(b, r1_local);
        }
        wb_u8(b, (op->flags & WASM_OP_FLAG_INVERT) ? 0xb1 : 0xb0);
        wb_local_set(b, dst);
        break;

    case WASM_OP_FTOF_TO_F32:
        dst = wasm_f64_reg_local(op->r0, local_f0);
        WB_GET_OR_SKIP(b, dst);
        wb_u8(b, 0xb6);
        wb_u8(b, 0xbb);
        WB_SET_OR_TEE(b, dst);
        break;

    case WASM_OP_CALL:
    {
        int i, fi = wasm_find_func_index_by_tok(op->call_tok);
        if (fi < 0)
            tcc_error("wasm32 backend: unresolved direct call");
        for (i = 0; i < op->call_nb_args; ++i)
            wasm_emit_call_arg(b, op, i, local_fp);
        wb_u8(b, 0x10), wb_uleb(b, fi);
        if (op->type == WASM_VAL_I32)
            wb_local_set(b, wasm_i32_reg_local(REG_IRET, local_i0));
        else if (op->type == WASM_VAL_I64)
            wb_local_set(b, wasm_i64_reg_local(REG_LRET, local_tmp64));
        else if (op->type == WASM_VAL_F32) {
            wb_u8(b, 0xbb);
            wb_local_set(b, wasm_f64_reg_local(REG_FRET, local_f0));
        } else if (op->type == WASM_VAL_F64)
            wb_local_set(b, wasm_f64_reg_local(REG_FRET, local_f0));
        break;
    }

    case WASM_OP_RET:
        if (op_to_block) {
            /* Direct br to halt block (always forward) */
            wb_u8(b, 0x0c), wb_uleb(b, loop_depth - 1);
        } else {
            wb_i32_const(b, f->nb_ops);
            wb_local_set(b, local_pc);
            wb_u8(b, 0x0c), wb_uleb(b, loop_depth);
        }
        return;

    default:
        tcc_error("wasm32 backend: unknown IR op kind %u", op->kind);
        break;
    }

    if (emit_dispatch) {
        int dispatch_target;
        if (op_to_block && next_index < f->nb_ops)
            dispatch_target = op_to_block[next_index];
        else if (op_to_block)
            dispatch_target = nb_blocks;
        else
            dispatch_target = next_index;
        *p_stack_out = -1;
        wb_i32_const(b, dispatch_target);
        wb_local_set(b, local_pc);
        wb_u8(b, 0x0c), wb_uleb(b, loop_depth);
    }
#undef WB_SET_OR_TEE
#undef WB_GET_OR_SKIP
}

static void wasm_emit_function_body(WasmBuf *code, WasmFuncIR *f, TCCState *s1)
{
    WasmBuf body;
    int i;
    int local_pc, local_fp, local_cmp, local_carry, local_i0, local_f0, local_tmp64;

    memset(&body, 0, sizeof(body));

    /* locals: control/i32 registers, f64 registers, native i64 registers
       plus one i64 scratch used by the remaining i32 carry helpers. */
    wb_uleb(&body, 3);
    wb_uleb(&body, 8), wb_u8(&body, 0x7f);
    wb_uleb(&body, 4), wb_u8(&body, 0x7c);
    wb_uleb(&body, 5), wb_u8(&body, 0x7e);

    local_pc = f->nb_params;
    local_fp = local_pc + 1;
    local_cmp = local_fp + 1;
    local_carry = local_cmp + 1;
    local_i0 = local_carry + 1;
    local_f0 = local_i0 + 4;
    local_tmp64 = local_f0 + 8;

    /* Prolog: keep fp at the caller-visible stack top. The wasm backend
     * addresses frame slots using negative offsets, so fp must point to the
     * original stack pointer before reserving the current frame. */
    if (f->frame_size) {
        wb_global_get(&body, 0);
        wb_local_tee(&body, local_fp);
        wb_i32_const(&body, f->frame_size);
        wb_u8(&body, 0x6b); /* i32.sub */
        wb_global_set(&body, 0);
    } else {
        wb_global_get(&body, 0);
        wb_local_set(&body, local_fp);
    }

    wb_i32_const(&body, 0);
    wb_local_set(&body, local_cmp);
    wb_i32_const(&body, 0);
    wb_local_set(&body, local_carry);

    /* spill wasm params to linear-memory frame slots */
    for (i = 0; i < f->nb_params; ++i) {
        wb_local_get(&body, local_fp);
        if (f->param_offsets[i])
            wb_i32_const(&body, f->param_offsets[i]), wb_u8(&body, 0x6a);
        wb_local_get(&body, i);
        if (f->param_types[i] == WASM_VAL_I32)
            wb_u8(&body, 0x36), wb_memarg(&body, 2);
        else if (f->param_types[i] == WASM_VAL_I64)
            wb_u8(&body, 0x37), wb_memarg(&body, 3);
        else if (f->param_types[i] == WASM_VAL_F32)
            wb_u8(&body, 0x38), wb_memarg(&body, 2);
        else if (f->param_types[i] == WASM_VAL_F64)
            wb_u8(&body, 0x39), wb_memarg(&body, 3);
        else
            tcc_error("wasm32 backend: bad parameter type %d", f->param_types[i]);
    }

    if (f->nb_ops > 0) {
        /* --- Basic block coalescing ---
         * Identify basic block leaders and merge consecutive non-branching
         * ops into single br_table cases to reduce dispatch overhead. */
        int *is_leader = tcc_mallocz(f->nb_ops * sizeof(int));
        int *op_to_block = tcc_mallocz(f->nb_ops * sizeof(int));
        int *block_start; /* first op index of each block */
        int *block_end;   /* one past last op index of each block */
        int nb_blocks = 0, b_idx;

        /* 1. Identify basic block leaders */
        is_leader[0] = 1;
        for (i = 0; i < f->nb_ops; i++) {
            WasmOp *op = &f->ops[i];
            if (op->kind == WASM_OP_JMP || op->kind == WASM_OP_JMP_CMP) {
                int target = wasm_pc_to_index(f, op->target_pc);
                if (target >= 0 && target < f->nb_ops)
                    is_leader[target] = 1;
                if (i + 1 < f->nb_ops)
                    is_leader[i + 1] = 1;
            } else if (op->kind == WASM_OP_RET) {
                if (i + 1 < f->nb_ops)
                    is_leader[i + 1] = 1;
            }
        }

        /* 2. Assign block indices */
        for (i = 0; i < f->nb_ops; i++) {
            if (is_leader[i]) nb_blocks++;
            op_to_block[i] = nb_blocks - 1;
        }

        block_start = tcc_mallocz(nb_blocks * sizeof(int));
        block_end = tcc_mallocz(nb_blocks * sizeof(int));
        b_idx = -1;
        for (i = 0; i < f->nb_ops; i++) {
            if (is_leader[i]) {
                b_idx++;
                block_start[b_idx] = i;
            }
            block_end[b_idx] = i + 1;
        }

        tcc_free(is_leader);

        /* --- Structured control flow reconstruction (Stackifier) ---
         *
         * Instead of a switch-loop dispatch, emit proper wasm block/loop/br
         * constructs that mirror the C source's control flow.  This eliminates
         * all dispatch overhead for reducible CFGs (which C always produces).
         *
         * Algorithm:
         *  1. For each basic block, determine terminator type and successors
         *  2. Identify loop headers (blocks targeted by backward edges)
         *  3. For forward branches, determine where to place block scopes
         *  4. Walk blocks in order, opening/closing scopes and emitting branches
         *
         * Scope stack entries are either LOOP or BLOCK.  A br to a LOOP scope
         * continues the loop; a br to a BLOCK scope exits forward.
         */

        /* Determine terminator kind and successors for each block */
        int *blk_succ0 = tcc_mallocz(nb_blocks * sizeof(int));  /* primary successor */
        int *blk_succ1 = tcc_mallocz(nb_blocks * sizeof(int));  /* secondary (JMP_CMP fallthrough) */
        int *blk_term  = tcc_mallocz(nb_blocks * sizeof(int));  /* terminator kind */
        int *blk_term_flags = tcc_mallocz(nb_blocks * sizeof(int));  /* terminator op flags */

        for (b_idx = 0; b_idx < nb_blocks; b_idx++) {
            int last_op_idx = block_end[b_idx] - 1;
            WasmOp *last_op = &f->ops[last_op_idx];
            blk_succ0[b_idx] = -1;
            blk_succ1[b_idx] = -1;
            blk_term[b_idx] = last_op->kind;
            blk_term_flags[b_idx] = last_op->flags;

            if (last_op->kind == WASM_OP_JMP) {
                int ti = wasm_pc_to_index(f, last_op->target_pc);
                blk_succ0[b_idx] = (ti < f->nb_ops) ? op_to_block[ti] : nb_blocks;
            } else if (last_op->kind == WASM_OP_JMP_CMP) {
                int ti = wasm_pc_to_index(f, last_op->target_pc);
                int ni = last_op_idx + 1;
                blk_succ0[b_idx] = (ti < f->nb_ops) ? op_to_block[ti] : nb_blocks;
                blk_succ1[b_idx] = (ni < f->nb_ops) ? op_to_block[ni] : nb_blocks;
            } else if (last_op->kind == WASM_OP_RET) {
                blk_succ0[b_idx] = nb_blocks; /* halt */
            } else {
                /* Non-control-flow: falls through to next block */
                blk_succ0[b_idx] = (b_idx + 1 < nb_blocks) ? b_idx + 1 : nb_blocks;
            }
        }

        /* Identify loop headers: a block is a loop header if any block with
         * a higher index jumps to it (backward edge).
         * Also compute loop_end[h] = the last block that has a back-edge to h. */
        int *is_loop_header = tcc_mallocz(nb_blocks * sizeof(int));
        int *loop_end = tcc_mallocz(nb_blocks * sizeof(int));
        for (b_idx = 0; b_idx < nb_blocks; b_idx++) {
            if (blk_succ0[b_idx] >= 0 && blk_succ0[b_idx] < nb_blocks
                && blk_succ0[b_idx] <= b_idx) {
                is_loop_header[blk_succ0[b_idx]] = 1;
                if (b_idx > loop_end[blk_succ0[b_idx]])
                    loop_end[blk_succ0[b_idx]] = b_idx;
            }
            if (blk_succ1[b_idx] >= 0 && blk_succ1[b_idx] < nb_blocks
                && blk_succ1[b_idx] <= b_idx) {
                is_loop_header[blk_succ1[b_idx]] = 1;
                if (b_idx > loop_end[blk_succ1[b_idx]])
                    loop_end[blk_succ1[b_idx]] = b_idx;
            }
        }
        /* Extend loop_end to cover nested loops: if inner loop header h2 is
         * inside outer loop h1 (h1 <= h2 <= loop_end[h1]), then outer loop
         * must extend to at least loop_end[h2]. Iterate until stable. */
        {
            int changed;
            do {
                changed = 0;
                int h1, h2;
                for (h1 = 0; h1 < nb_blocks; h1++) {
                    if (!is_loop_header[h1]) continue;
                    for (h2 = h1 + 1; h2 <= loop_end[h1]; h2++) {
                        if (!is_loop_header[h2]) continue;
                        if (loop_end[h2] > loop_end[h1]) {
                            loop_end[h1] = loop_end[h2];
                            changed = 1;
                        }
                    }
                }
            } while (changed);
        }

        /* Detect "jump into loop" patterns: a forward edge from block b
         * to block t where some loop header h has b < h <= t <= loop_end[h].
         * This pattern (common in TCC's for-loop layout) cannot be directly
         * expressed in wasm structured control flow.  Fall back to the
         * switch-loop dispatch for such functions. */
        int use_structured = 1;
        for (b_idx = 0; b_idx < nb_blocks && use_structured; b_idx++) {
            int succs[2], ns2 = 0;
            if (blk_succ0[b_idx] > b_idx && blk_succ0[b_idx] < nb_blocks)
                succs[ns2++] = blk_succ0[b_idx];
            if (blk_succ1[b_idx] > b_idx && blk_succ1[b_idx] < nb_blocks)
                succs[ns2++] = blk_succ1[b_idx];
            for (i = 0; i < ns2; i++) {
                int target = succs[i];
                int h;
                for (h = b_idx + 1; h < target; h++) {
                    if (is_loop_header[h] && target <= loop_end[h]) {
                        use_structured = 0;
                        break;
                    }
                }
            }
        }

        if (use_structured) {

        /* For each forward-branch target, find where to open its block scope.
         *
         * fwd_scope_open[t] = block index where the scope for target t opens.
         * The scope closes (end instruction) just before block t's code.
         *
         * Key constraint: if a forward branch crosses a loop boundary (source
         * is inside a loop but target is outside), the scope must open BEFORE
         * the loop (at or before the loop header), not inside the loop body.
         * Otherwise the scope would cross the loop boundary, violating nesting.
         */
        int *fwd_scope_open = tcc_mallocz(nb_blocks * sizeof(int));
        int *needs_fwd_scope = tcc_mallocz(nb_blocks * sizeof(int));

        /* Compute innermost_loop[b] = loop header containing b, or -1 */
        int *innermost_loop = tcc_mallocz(nb_blocks * sizeof(int));
        {
            int h;
            for (i = 0; i < nb_blocks; i++)
                innermost_loop[i] = -1;
            /* For each loop header h, mark blocks h..loop_end[h] */
            for (h = 0; h < nb_blocks; h++) {
                if (!is_loop_header[h]) continue;
                for (i = h; i <= loop_end[h]; i++) {
                    /* Keep the innermost (most recently opened) loop.
                     * Since we iterate h in ascending order, later h
                     * overwrites earlier h for nested loops. */
                    if (innermost_loop[i] < h)
                        innermost_loop[i] = h;
                }
            }
        }

        for (i = 0; i < nb_blocks; i++)
            fwd_scope_open[i] = nb_blocks; /* sentinel */

        for (b_idx = 0; b_idx < nb_blocks; b_idx++) {
            int s0 = blk_succ0[b_idx], s1 = blk_succ1[b_idx];
            int succs[2], ns = 0;
            if (s0 > b_idx && s0 < nb_blocks) succs[ns++] = s0;
            if (s1 > b_idx && s1 < nb_blocks) succs[ns++] = s1;

            for (i = 0; i < ns; i++) {
                int target = succs[i];
                /* Fallthrough to next block never needs a scope */
                if (target == b_idx + 1)
                    continue;

                needs_fwd_scope[target] = 1;

                /* Determine where to open the scope.  Start at source block,
                 * then adjust outward past any loop boundaries. */
                int open_at = b_idx;
                {
                    /* If target is outside any loop containing b_idx,
                     * push open_at to before that loop's header. */
                    int cur = b_idx;
                    while (innermost_loop[cur] >= 0) {
                        int h = innermost_loop[cur];
                        if (target > loop_end[h]) {
                            /* Target is outside this loop — scope must
                             * open at or before the loop header */
                            if (h < open_at)
                                open_at = h;
                            /* Check if the loop header is itself inside
                             * an outer loop */
                            if (h > 0 && innermost_loop[h - 1] >= 0
                                && innermost_loop[h - 1] < h)
                                cur = h - 1;
                            else
                                break;
                        } else {
                            break;
                        }
                    }
                }
                if (open_at < fwd_scope_open[target])
                    fwd_scope_open[target] = open_at;
            }
        }

        /* Fixpoint: ensure forward-branch scopes nest properly.
         *
         * If scope for target t1 (open=o1, close=t1) contains the opening
         * point of scope for target t2 (open=o2, close=t2), and t2 > t1,
         * then t2's range crosses t1's boundary.  Fix by extending t2's
         * opening to at or before o1 so that t2 becomes the outer scope.
         *
         * Iterate until no changes occur. */
        {
            int changed2;
            do {
                changed2 = 0;
                for (i = 0; i < nb_blocks; i++) {
                    int t2;
                    if (!needs_fwd_scope[i]) continue;
                    /* scope for target i: opens at fwd_scope_open[i], closes at i */
                    for (t2 = i + 1; t2 < nb_blocks; t2++) {
                        if (!needs_fwd_scope[t2]) continue;
                        /* scope for target t2: opens at fwd_scope_open[t2], closes at t2 */
                        if (fwd_scope_open[t2] >= fwd_scope_open[i]
                            && fwd_scope_open[t2] < i
                            && t2 > i) {
                            /* t2 opens inside scope i but closes after i.
                             * Move t2's open to before scope i's open. */
                            if (fwd_scope_open[i] < fwd_scope_open[t2]) {
                                fwd_scope_open[t2] = fwd_scope_open[i];
                                changed2 = 1;
                            }
                        }
                    }
                }
            } while (changed2);
        }

        /* Now emit the structured code.
         *
         * We maintain a scope stack.  Each entry records:
         *  - type: 'B' (block) or 'L' (loop)
         *  - target: which block index this scope serves
         *    For BLOCK: br exits to block 'target' (scope ends before target)
         *    For LOOP: br continues loop 'target' (scope starts at target)
         *
         * Outermost scope is a BLOCK for the function exit (halt).
         */

        /* Scope stack */
        int scope_cap = nb_blocks * 2 + 4;
        int *scope_type = tcc_mallocz(scope_cap * sizeof(int));   /* 'B' or 'L' */
        int *scope_target = tcc_mallocz(scope_cap * sizeof(int)); /* block index */
        int scope_depth = 0;

        /* Push halt block scope */
        wb_u8(&body, 0x02), wb_u8(&body, 0x40); /* block (halt) */
        scope_type[scope_depth] = 'B';
        scope_target[scope_depth] = nb_blocks; /* halt */
        scope_depth++;

        for (b_idx = 0; b_idx < nb_blocks; b_idx++) {
            int j, stack_reg = -1;

#if 0
            fprintf(stderr, "BLOCK %d: ops %d-%d, succ0=%d succ1=%d term=%d loop_hdr=%d\n",
                b_idx, block_start[b_idx], block_end[b_idx]-1,
                blk_succ0[b_idx], blk_succ1[b_idx], blk_term[b_idx],
                is_loop_header[b_idx]);
            if (is_loop_header[b_idx])
                fprintf(stderr, "  loop_end=%d\n", loop_end[b_idx]);
            if (needs_fwd_scope[b_idx])
                fprintf(stderr, "  needs_fwd_scope, open_at=%d\n", fwd_scope_open[b_idx]);
            { int dd; fprintf(stderr, "  scope stack (%d):", scope_depth);
              for (dd = 0; dd < scope_depth; dd++)
                fprintf(stderr, " %c%d", scope_type[dd] == 'L' ? 'L' : 'B', scope_target[dd]);
              fprintf(stderr, "\n"); }
            { int jj;
              for (jj = block_start[b_idx]; jj < block_end[b_idx]; jj++) {
                WasmOp *o = &f->ops[jj];
                fprintf(stderr, "  op[%d] kind=%d", jj, o->kind);
                if (o->kind == WASM_OP_JMP || o->kind == WASM_OP_JMP_CMP)
                    fprintf(stderr, " target_pc=%d target_idx=%d flags=0x%x", o->target_pc,
                        wasm_pc_to_index(f, o->target_pc), o->flags);
                fprintf(stderr, "\n");
              }
            }
#endif

            /* Close scopes that end before this block */
            while (scope_depth > 0) {
                int top = scope_depth - 1;
                if (scope_type[top] == 'B' && scope_target[top] == b_idx) {
                    wb_u8(&body, 0x0b); /* end */
                    scope_depth--;
                } else {
                    break;
                }
            }

            /* Open block scopes and loop scope.
             *
             * Block scopes whose targets are OUTSIDE any loop at this block
             * must be opened BEFORE the loop scope.  Block scopes whose
             * targets are INSIDE the loop are opened AFTER the loop scope. */
            {
                int targets_outer[64], n_outer = 0;
                int targets_inner[64], n_inner = 0;
                int t;
                int le = is_loop_header[b_idx] ? loop_end[b_idx] : -1;

                for (t = b_idx + 1; t < nb_blocks; t++) {
                    if (needs_fwd_scope[t] && fwd_scope_open[t] == b_idx) {
                        if (is_loop_header[b_idx] && t <= le && n_inner < 64)
                            targets_inner[n_inner++] = t;
                        else if (n_outer < 64)
                            targets_outer[n_outer++] = t;
                    }
                }

                /* Open outer scopes (outside loop): furthest first */
                for (j = n_outer - 1; j >= 0; j--) {
                    wb_u8(&body, 0x02), wb_u8(&body, 0x40);
                    scope_type[scope_depth] = 'B';
                    scope_target[scope_depth] = targets_outer[j];
                    scope_depth++;
                }

                /* Open loop scope */
                if (is_loop_header[b_idx]) {
                    wb_u8(&body, 0x03), wb_u8(&body, 0x40);
                    scope_type[scope_depth] = 'L';
                    scope_target[scope_depth] = b_idx;
                    scope_depth++;
                }

                /* Open inner scopes (inside loop): furthest first */
                for (j = n_inner - 1; j >= 0; j--) {
                    wb_u8(&body, 0x02), wb_u8(&body, 0x40);
                    scope_type[scope_depth] = 'B';
                    scope_target[scope_depth] = targets_inner[j];
                    scope_depth++;
                }
            }

            /* Emit non-terminal ops of this block */
            for (j = block_start[b_idx]; j < block_end[b_idx]; ++j) {
                WasmOp *op = &f->ops[j];
                int is_terminal = (j == block_end[b_idx] - 1) &&
                    (op->kind == WASM_OP_JMP || op->kind == WASM_OP_JMP_CMP ||
                     op->kind == WASM_OP_RET);

                if (is_terminal) {
                    /* Emit terminator branch using scope stack */
                    if (op->kind == WASM_OP_RET) {
                        /* Find halt scope depth */
                        int d;
                        for (d = scope_depth - 1; d >= 0; d--) {
                            if (scope_type[d] == 'B' && scope_target[d] == nb_blocks)
                                break;
                        }
                        wb_u8(&body, 0x0c);
                        wb_uleb(&body, scope_depth - 1 - d);
                    } else if (op->kind == WASM_OP_JMP) {
                        int target_bi = blk_succ0[b_idx];
                        if (target_bi == b_idx + 1) {
                            /* JMP to next block: natural fallthrough */
                        } else {
                            int d;
                            if (target_bi <= b_idx && target_bi < nb_blocks) {
                                /* Backward: find loop scope */
                                for (d = scope_depth - 1; d >= 0; d--) {
                                    if (scope_type[d] == 'L' && scope_target[d] == target_bi)
                                        break;
                                }
                            } else {
                                /* Forward: find block scope */
                                for (d = scope_depth - 1; d >= 0; d--) {
                                    if (scope_type[d] == 'B' && scope_target[d] == target_bi)
                                        break;
                                }
                            }
                            if (d < 0)
                                tcc_error("wasm32 structured: no scope for JMP target block %d from block %d", target_bi, b_idx);
                            wb_u8(&body, 0x0c);
                            wb_uleb(&body, scope_depth - 1 - d);
                        }
                    } else { /* WASM_OP_JMP_CMP */
                        int taken_bi = blk_succ0[b_idx];
                        /* fall_bi is always b_idx + 1 (next sequential block) */
                        int d;

                        /* Load condition */
                        wb_local_get(&body, local_cmp);
                        if (op->flags & WASM_OP_FLAG_INVERT)
                            wb_u8(&body, 0x45); /* i32.eqz */

                        /* Find scope for taken target */
                        if (taken_bi <= b_idx && taken_bi < nb_blocks) {
                            for (d = scope_depth - 1; d >= 0; d--)
                                if (scope_type[d] == 'L' && scope_target[d] == taken_bi) break;
                        } else {
                            for (d = scope_depth - 1; d >= 0; d--)
                                if (scope_type[d] == 'B' && scope_target[d] == taken_bi) break;
                        }
                        if (d < 0)
                            tcc_error("wasm32 structured: no scope for JMP_CMP taken block %d from block %d", taken_bi, b_idx);
                        wb_u8(&body, 0x0d); /* br_if */
                        wb_uleb(&body, scope_depth - 1 - d);
                        /* Not taken: fallthrough to b_idx + 1 (natural) */
                    }
                } else {
                    /* Non-terminal op: emit normally */
                    int is_last_in_block = (j == block_end[b_idx] - 1);
                    int next_fi = -1;
                    int stack_out = -1;
                    if (!is_last_in_block && j + 1 < block_end[b_idx])
                        next_fi = wasm_op_first_input(&f->ops[j + 1], local_i0, local_f0);
                    /* Pass op_to_block=NULL, emit_dispatch=0 to suppress
                     * all control flow emission in wasm_emit_case */
                    wasm_emit_case(&body, f, op, j, 0, 0,
                                   local_pc, local_fp, local_cmp, local_carry,
                                   local_i0, local_f0, local_tmp64,
                                   NULL, 0, 0,
                                   stack_reg, next_fi, &stack_out);
                    stack_reg = stack_out;
                    if (s1->nb_errors) break;
                }
            }
            if (s1->nb_errors) break;

            /* Non-control-flow block endings naturally fall through to the
             * next block.  Wasm 'end' instructions for closing scopes don't
             * redirect fallthrough, so no explicit branch is needed. */

            /* Close loop scopes whose range ends at this block.
             * Close from the top of the stack (innermost first). */
            while (scope_depth > 0) {
                int top = scope_depth - 1;
                if (scope_type[top] == 'L' && loop_end[scope_target[top]] == b_idx) {
                    wb_u8(&body, 0x0b); /* end loop */
                    scope_depth--;
                } else {
                    break;
                }
            }
        }

        /* Close remaining scopes */
        while (scope_depth > 0) {
            wb_u8(&body, 0x0b); /* end */
            scope_depth--;
        }

        tcc_free(scope_type);
        tcc_free(scope_target);
        tcc_free(innermost_loop);
        tcc_free(fwd_scope_open);
        tcc_free(needs_fwd_scope);

        } else {
            /* --- Fallback: switch-loop dispatch with coalescing ---
             * Used when the function has "jump into loop" patterns that
             * can't be expressed in wasm structured control flow. */
            wb_i32_const(&body, 0);
            wb_local_set(&body, local_pc);

            wb_u8(&body, 0x03), wb_u8(&body, 0x40); /* loop */
            wb_u8(&body, 0x02), wb_u8(&body, 0x40); /* halt block */

            for (i = 0; i < nb_blocks; ++i)
                wb_u8(&body, 0x02), wb_u8(&body, 0x40);

            wb_local_get(&body, local_pc);
            wb_u8(&body, 0x0e);
            wb_uleb(&body, nb_blocks);
            for (i = 0; i < nb_blocks; ++i)
                wb_uleb(&body, nb_blocks - 1 - i);
            wb_uleb(&body, nb_blocks); /* default -> halt */

            for (b_idx = nb_blocks - 1; b_idx >= 0; --b_idx) {
                int j, stack_reg = -1;
                wb_u8(&body, 0x0b); /* end block */
                for (j = block_start[b_idx]; j < block_end[b_idx]; ++j) {
                    int is_last = (j == block_end[b_idx] - 1);
                    int next_fi = -1;
                    int stack_out = -1;
                    if (!is_last && j + 1 < block_end[b_idx])
                        next_fi = wasm_op_first_input(&f->ops[j + 1], local_i0, local_f0);
                    wasm_emit_case(&body, f, &f->ops[j], j, b_idx + 1, b_idx,
                                   local_pc, local_fp, local_cmp, local_carry,
                                   local_i0, local_f0, local_tmp64,
                                   op_to_block, nb_blocks, is_last,
                                   stack_reg, next_fi, &stack_out);
                    stack_reg = stack_out;
                    if (s1->nb_errors) break;
                }
                if (s1->nb_errors) break;
            }

            wb_u8(&body, 0x0b); /* end halt */
            wb_u8(&body, 0x0b); /* end loop */
        }

        tcc_free(blk_succ0);
        tcc_free(blk_succ1);
        tcc_free(blk_term);
        tcc_free(blk_term_flags);
        tcc_free(is_loop_header);
        tcc_free(loop_end);
        tcc_free(op_to_block);
        tcc_free(block_start);
        tcc_free(block_end);
    }

    if (s1->nb_errors) {
        tcc_free(body.data);
        return;
    }

    /* epilog: restore stack pointer */
    wb_local_get(&body, local_fp);
    wb_global_set(&body, 0);

    if (f->ret_type == WASM_VAL_I32)
        wb_local_get(&body, wasm_i32_reg_local(REG_IRET, local_i0));
    else if (f->ret_type == WASM_VAL_I64)
        wb_local_get(&body, wasm_i64_reg_local(REG_LRET, local_tmp64));
    else if (f->ret_type == WASM_VAL_F32)
        wb_local_get(&body, wasm_f64_reg_local(REG_FRET, local_f0)), wb_u8(&body, 0xb6);
    else if (f->ret_type == WASM_VAL_F64)
        wb_local_get(&body, wasm_f64_reg_local(REG_FRET, local_f0));

    wb_u8(&body, 0x0b); /* end function body */

    wb_uleb(code, body.len);
    wb_mem(code, body.data, body.len);
    tcc_free(body.data);
}

static void wasm_add_section(WasmBuf *mod, int id, WasmBuf *sec)
{
    if (sec->len == 0)
        return;
    wb_u8(mod, id);
    wb_uleb(mod, sec->len);
    wb_mem(mod, sec->data, sec->len);
}

static void wasm_str(WasmBuf *b, const char *s)
{
    int n = (int)strlen(s);
    wb_uleb(b, n);
    wb_mem(b, s, n);
}

ST_FUNC int tcc_output_wasm(TCCState *s1, const char *filename)
{
    WasmBuf mod, sec_type, sec_func, sec_mem, sec_glob, sec_exp, sec_code, sec_data;
    int fd, i, nb_exports;
    int ro_size, data_size, bss_size, stack_size, memory_pages;
    int cur;
    TCCState *old_state = tcc_state;

    memset(&mod, 0, sizeof(mod));
    memset(&sec_type, 0, sizeof(sec_type));
    memset(&sec_func, 0, sizeof(sec_func));
    memset(&sec_mem, 0, sizeof(sec_mem));
    memset(&sec_glob, 0, sizeof(sec_glob));
    memset(&sec_exp, 0, sizeof(sec_exp));
    memset(&sec_code, 0, sizeof(sec_code));
    memset(&sec_data, 0, sizeof(sec_data));

    tcc_state = s1;
    wasm_sec_text = text_section;
    wasm_sec_data = data_section;
    wasm_sec_rodata = NULL;
    wasm_sec_bss = bss_section;

    ro_size = wasm_sec_rodata ? (int)wasm_sec_rodata->data_offset : 0;
    data_size = wasm_sec_data ? (int)wasm_sec_data->data_offset : 0;
    bss_size = wasm_sec_bss ? (int)wasm_sec_bss->data_offset : 0;
    stack_size = 65536;

    cur = 1024;
    wasm_layout.rodata_base = ro_size ? wasm_align_up(cur, 16) : 0;
    if (ro_size) cur = wasm_layout.rodata_base + ro_size;
    wasm_layout.data_base = data_size ? wasm_align_up(cur, 16) : 0;
    if (data_size) cur = wasm_layout.data_base + data_size;
    wasm_layout.bss_base = wasm_align_up(cur, 16);
    cur = wasm_layout.bss_base + bss_size;
    wasm_layout.stack_top = wasm_align_up(cur + stack_size, 16);
    memory_pages = (wasm_layout.stack_top + 65535) / 65536;

    if (wasm_validate_calls() < 0 || s1->nb_errors) {
        tcc_state = old_state;
        return -1;
    }

    /* Resolve static data initializers that require symbol addresses. */
    wasm_apply_data_relocs(wasm_sec_rodata);
    wasm_apply_data_relocs(wasm_sec_data);

    /* type section: one type per function. */
    wb_uleb(&sec_type, tcc_wasm_nb_funcs);
    for (i = 0; i < tcc_wasm_nb_funcs; ++i) {
        WasmFuncIR *f = &tcc_wasm_funcs[i];
        int j;
        wb_u8(&sec_type, 0x60);
        wb_uleb(&sec_type, f->nb_params);
        for (j = 0; j < f->nb_params; ++j)
            wb_u8(&sec_type, wasm_valtype_byte(f->param_types[j]));
        if (f->ret_type == WASM_VAL_VOID)
            wb_uleb(&sec_type, 0);
        else
            wb_uleb(&sec_type, 1), wb_u8(&sec_type, wasm_valtype_byte(f->ret_type));
    }
    /* function section */
    wb_uleb(&sec_func, tcc_wasm_nb_funcs);
    for (i = 0; i < tcc_wasm_nb_funcs; ++i)
        wb_uleb(&sec_func, i);

    /* memory section: one linear memory */
    wb_uleb(&sec_mem, 1);
    wb_u8(&sec_mem, 0x00); /* limits: min only */
    wb_uleb(&sec_mem, memory_pages);

    /* mutable global __stack_pointer */
    wb_uleb(&sec_glob, 1);
    wb_u8(&sec_glob, 0x7f); /* i32 */
    wb_u8(&sec_glob, 0x01); /* mutable */
    wb_i32_const(&sec_glob, wasm_layout.stack_top);
    wb_u8(&sec_glob, 0x0b);

    /* exports: memory + non-static functions */
    nb_exports = 1;
    for (i = 0; i < tcc_wasm_nb_funcs; ++i) {
        if (tcc_wasm_funcs[i].is_static)
            continue;
        if (!tcc_wasm_funcs[i].name || !*tcc_wasm_funcs[i].name)
            continue;
        nb_exports++;
    }
    wb_uleb(&sec_exp, nb_exports);

    wasm_str(&sec_exp, "memory");
    wb_u8(&sec_exp, 0x02), wb_uleb(&sec_exp, 0);

    for (i = 0; i < tcc_wasm_nb_funcs; ++i) {
        const char *name;
        if (tcc_wasm_funcs[i].is_static)
            continue;
        name = tcc_wasm_funcs[i].name;
        if (!name || !*name)
            continue;
        wasm_str(&sec_exp, name);
        wb_u8(&sec_exp, 0x00);
        wb_uleb(&sec_exp, i);
    }

    /* code section */
    wb_uleb(&sec_code, tcc_wasm_nb_funcs);
    for (i = 0; i < tcc_wasm_nb_funcs; ++i) {
        wasm_emit_function_body(&sec_code, &tcc_wasm_funcs[i], s1);
        if (s1->nb_errors) break;
    }

    if (s1->nb_errors) {
        tcc_state = old_state;
        return -1;
    }

    /* data section (rodata + data). bss remains zero-initialized */
    if (ro_size || data_size) {
        int segs = (ro_size ? 1 : 0) + (data_size ? 1 : 0);
        wb_uleb(&sec_data, segs);
        if (ro_size) {
            wb_u8(&sec_data, 0x00);
            wb_i32_const(&sec_data, wasm_layout.rodata_base);
            wb_u8(&sec_data, 0x0b);
            wb_uleb(&sec_data, ro_size);
            wb_mem(&sec_data, wasm_sec_rodata->data, ro_size);
        }
        if (data_size) {
            wb_u8(&sec_data, 0x00);
            wb_i32_const(&sec_data, wasm_layout.data_base);
            wb_u8(&sec_data, 0x0b);
            wb_uleb(&sec_data, data_size);
            wb_mem(&sec_data, wasm_sec_data->data, data_size);
        }
    }

    /* module header */
    wb_u8(&mod, 0x00), wb_u8(&mod, 0x61), wb_u8(&mod, 0x73), wb_u8(&mod, 0x6d);
    wb_u8(&mod, 0x01), wb_u8(&mod, 0x00), wb_u8(&mod, 0x00), wb_u8(&mod, 0x00);

    wasm_add_section(&mod, 1, &sec_type);
    wasm_add_section(&mod, 3, &sec_func);
    wasm_add_section(&mod, 5, &sec_mem);
    wasm_add_section(&mod, 6, &sec_glob);
    wasm_add_section(&mod, 7, &sec_exp);
    wasm_add_section(&mod, 10, &sec_code);
    wasm_add_section(&mod, 11, &sec_data);

    unlink(filename);
    fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
    if (fd < 0) {
        tcc_state = old_state;
        tcc_error_noabort("could not write '%s: %s'", filename, strerror(errno));
        return -1;
    }
    if (write(fd, mod.data, mod.len) != (ssize_t)mod.len) {
        close(fd);
        tcc_state = old_state;
        tcc_error_noabort("could not write '%s: %s'", filename, strerror(errno));
        return -1;
    }
    close(fd);

    if (s1->verbose)
        printf("<- %s\n", filename);

    tcc_free(mod.data);
    tcc_free(sec_type.data);
    tcc_free(sec_func.data);
    tcc_free(sec_mem.data);
    tcc_free(sec_glob.data);
    tcc_free(sec_exp.data);
    tcc_free(sec_code.data);
    tcc_free(sec_data.data);

    tcc_state = old_state;
    return 0;
}
