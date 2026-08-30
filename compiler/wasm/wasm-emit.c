/* Encoding primitives for the wasm binary format: the growable byte
   buffer, LEB128 and float encoders, the instruction shorthands, and the
   opcode tables.
   These used to be private to tccwasm.c, which was the only place bytes
   were produced -- wasm-gen.c recorded a symbolic op and tccwasm.c later
   interpreted it. wasm-gen.c now encodes instructions as it generates
   them, so both files need these. */
#define USING_GLOBALS
#include "../tcc.h"
#include "wasm-backend.h"
ST_FUNC int wasm_align_up(int v, int a)
{
    return (v + a - 1) & -a;
}
ST_FUNC void wb_reserve(WasmBuf *b, int add)
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
ST_FUNC void wb_u8(WasmBuf *b, int v)
{
    wb_reserve(b, 1);
    b->data[b->len++] = (unsigned char)v;
}
ST_FUNC void wb_mem(WasmBuf *b, const void *p, int n)
{
    wb_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
ST_FUNC void wb_uleb(WasmBuf *b, unsigned v)
{
    do {
        unsigned c = v & 0x7f;
        v >>= 7;
        if (v)
            c |= 0x80;
        wb_u8(b, c);
    } while (v);
}
ST_FUNC void wb_sleb(WasmBuf *b, int v)
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
ST_FUNC void wb_sleb64(WasmBuf *b, int64_t v)
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
ST_FUNC void wb_f64(WasmBuf *b, double x)
{
    union {
        double d;
        unsigned char c[8];
    } u;
    u.d = x;
    wb_mem(b, u.c, 8);
}
ST_FUNC int wasm_valtype_byte(int t)
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
ST_FUNC void wb_local_get(WasmBuf *b, int idx) { wb_u8(b, 0x20), wb_uleb(b, idx); }
ST_FUNC void wb_local_set(WasmBuf *b, int idx) { wb_u8(b, 0x21), wb_uleb(b, idx); }
ST_FUNC void wb_local_tee(WasmBuf *b, int idx) { wb_u8(b, 0x22), wb_uleb(b, idx); }
ST_FUNC void wb_global_get(WasmBuf *b, int idx) { wb_u8(b, 0x23), wb_uleb(b, idx); }
ST_FUNC void wb_global_set(WasmBuf *b, int idx) { wb_u8(b, 0x24), wb_uleb(b, idx); }
ST_FUNC void wb_i32_const(WasmBuf *b, int v) { wb_u8(b, 0x41), wb_sleb(b, v); }
ST_FUNC void wb_i64_const(WasmBuf *b, int64_t v) { wb_u8(b, 0x42), wb_sleb64(b, v); }
ST_FUNC void wb_f64_const(WasmBuf *b, double v) { wb_u8(b, 0x44), wb_f64(b, v); }
ST_FUNC void wb_memarg(WasmBuf *b, int align_log2)
{
    wb_uleb(b, align_log2);
    wb_uleb(b, 0);
}
ST_FUNC int wasm_i32_bin_opcode(int op)
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
ST_FUNC int wasm_i32_cmp_opcode(int op)
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
ST_FUNC int wasm_i64_bin_opcode(int op)
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
ST_FUNC int wasm_i64_cmp_opcode(int op)
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
ST_FUNC int wasm_f32_cmp_opcode(int op)
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
ST_FUNC int wasm_f64_cmp_opcode(int op)
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
ST_FUNC int wasm_f_bin_opcode(int op, int is_f32)
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
