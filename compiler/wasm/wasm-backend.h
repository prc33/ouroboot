/* Adapted from Blosc/MiniCC's LGPL-2.1 WebAssembly backend:
   https://github.com/Blosc/minicc */
#ifndef TCC_WASM_BACKEND_H
#define TCC_WASM_BACKEND_H

/* Shared IR definitions between wasm-gen.c and tccwasm.c */

enum {
    WASM_VAL_VOID = 0,
    WASM_VAL_I32,
    WASM_VAL_I64,
    WASM_VAL_F32,
    WASM_VAL_F64,
};

enum {
    WASM_ADDR_REG = 0,
    WASM_ADDR_FP,
    WASM_ADDR_ABS,
    WASM_ADDR_SYM,
};

enum {
    WASM_OP_NOP = 0,
    WASM_OP_I32_CONST,
    WASM_OP_I64_CONST,
    WASM_OP_F64_CONST,
    WASM_OP_MOV_I32,
    WASM_OP_MOV_I64,
    WASM_OP_MOV_F64,
    WASM_OP_ADDR_LOCAL,
    WASM_OP_ADDR_SYM,
    WASM_OP_LOAD_I32,
    WASM_OP_LOAD_I64,
    WASM_OP_LOAD_I32_I64,
    WASM_OP_LOAD_S8,
    WASM_OP_LOAD_U8,
    WASM_OP_LOAD_S16,
    WASM_OP_LOAD_U16,
    WASM_OP_LOAD_F32,
    WASM_OP_LOAD_F64,
    WASM_OP_STORE_I32,
    WASM_OP_STORE_I64,
    WASM_OP_STORE_I8,
    WASM_OP_STORE_I16,
    WASM_OP_STORE_F32,
    WASM_OP_STORE_F64,
    WASM_OP_I32_BIN,
    WASM_OP_I64_BIN,
    WASM_OP_I32_NEG,
    WASM_OP_I64_NEG,
    WASM_OP_F64_BIN,
    WASM_OP_F32_BIN,
    WASM_OP_F64_NEG,
    WASM_OP_F32_NEG,
    WASM_OP_SET_CMP_I32,
    WASM_OP_SET_CMP_I64,
    WASM_OP_SET_CMP_F32,
    WASM_OP_SET_CMP_F64,
    WASM_OP_SET_I32_FROM_CMP,
    WASM_OP_JMP,
    WASM_OP_JMP_CMP,
    WASM_OP_ITOF_F32,
    WASM_OP_ITOF_F64,
    WASM_OP_I64_TOF_F32,
    WASM_OP_I64_TOF_F64,
    WASM_OP_FTOI_I32,
    WASM_OP_FTOI_I64,
    WASM_OP_FTOF_TO_F32,
    WASM_OP_EXTEND_I32_I64,
    WASM_OP_WRAP_I64_I32,
    WASM_OP_CALL,
    WASM_OP_RET,
};

#define WASM_OP_FLAG_IMM   0x0001
#define WASM_OP_FLAG_INVERT 0x0002
#define WASM_OP_FLAG_UNSIGNED 0x0100
/* WASM_OP_I32_BIN only: the second operand is not a register at all --
 * it's a plain `int`-sized local variable, still sitting at its own
 * frame slot, that gen_opi() deliberately never called gv() on. imm
 * holds its frame offset. Avoids ever allocating a fake "register" (a
 * wasm local slot) for the overwhelmingly common case of "combine
 * something with a local variable" -- see docs/wasm-backend-size-
 * 2026-08-28.md's own measurement of how much of the emitted module
 * was exactly this kind of avoidable local.get/local.set traffic. */
#define WASM_OP_FLAG_R1_LOCAL 0x0200
/* WASM_OP_I32_BIN / WASM_OP_SET_CMP_I32 only, and only combined with
 * WASM_OP_FLAG_R1_LOCAL: the FIRST operand is ALSO a plain local
 * (target_pc repurposed to hold its frame offset -- unused by these two
 * op kinds otherwise, which never jump anywhere). r0 stops being an
 * input at all here -- it's purely the destination -- so
 * wasm_op_first_input() must never report it as this op's first read
 * for this combination, or a preceding op could tee a value nothing
 * here consumes, leaving it stranded on the wasm stack (a genuine
 * validation-breaking imbalance, not just a missed optimization -- see
 * this flag's own emission-side comment). */
#define WASM_OP_FLAG_L_LOCAL 0x0400
/* WASM_OP_STORE_I32 / WASM_OP_STORE_I64 only: the value being stored never
 * got a register either -- gen_vstore_hook() (tcc.h/tccgen.c) intercepted
 * the assignment before tccgen.c's own vstore() ever called gv() to force
 * one. target_pc holds the source local's own frame offset, the same
 * repurposing WASM_OP_FLAG_L_LOCAL uses (STORE ops never jump, so
 * target_pc is otherwise unused by them). r0 is unused here -- there is
 * no source register at all. Mutually exclusive with
 * WASM_OP_FLAG_VAL_IMM. Bit chosen above 0x00ff deliberately: STORE's
 * `flags` low byte already holds the DESTINATION address mode
 * (WASM_ADDR_*, set by wasm_set_addr()) and must not be disturbed. */
#define WASM_OP_FLAG_VAL_LOCAL 0x0800
/* WASM_OP_STORE_I32 / WASM_OP_STORE_I64 only: the value being stored is a
 * compile-time constant, held in i64 (truncated to 32 bits by the emitter
 * for STORE_I32) -- also never given a register. Mutually exclusive with
 * WASM_OP_FLAG_VAL_LOCAL. */
#define WASM_OP_FLAG_VAL_IMM 0x1000

#define WASM_MAX_CALL_ARGS 32
typedef struct WasmOp {
    int pc;
    unsigned char kind;
    unsigned char type;
    unsigned short flags;
    int r0;
    int r1;
    int imm;
    int64_t i64;
    int op;
    int target_pc;
    double f64;
    int sym_index;
    int sym_tok;
    char *sym_name;
    int call_tok;
    char *call_name;
    unsigned char call_nb_args;
    unsigned char call_arg_type[WASM_MAX_CALL_ARGS];
    int call_arg_off[WASM_MAX_CALL_ARGS];
} WasmOp;

/* One while/for/do loop's exact extent, as tccgen.c itself knows it --
 * see gjmp_hint_loop_range()'s own comment in tcc.h. Recorded once per
 * loop construct, not once per repeat-edge, so a for-loop's rotated
 * layout (whose two individually-jumped-to positions -- the condition
 * test and the increment -- both belong to the SAME loop) still yields
 * exactly one range, not two. */
typedef struct WasmLoopRange {
    int start_pc, end_pc;
} WasmLoopRange;

typedef struct WasmFuncIR {
    int sym_tok;
    char *name;
    int start_pc;
    int end_pc;
    int frame_size;
    unsigned char ret_type;
    unsigned char has_sret;
    unsigned char is_static;
    int sret_param_offset;
    int nb_params;
    unsigned char *param_types;
    int *param_offsets;
    WasmOp *ops;
    int nb_ops;
    int cap_ops;
    WasmLoopRange *loops;
    int nb_loops;
    int cap_loops;
} WasmFuncIR;

extern WasmFuncIR *tcc_wasm_funcs;
extern int tcc_wasm_nb_funcs;

ST_FUNC void tcc_wasm_reset(void);

#endif /* TCC_WASM_BACKEND_H */
