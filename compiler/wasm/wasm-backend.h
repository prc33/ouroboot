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

#define WASM_MAX_CALL_ARGS 32
typedef struct WasmOp {
    int pc;
    unsigned char kind;
    unsigned char type;
    unsigned short flags;
    int r0;
    int r1;
    int r2;
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
} WasmFuncIR;

extern WasmFuncIR *tcc_wasm_funcs;
extern int tcc_wasm_nb_funcs;

ST_FUNC void tcc_wasm_reset(void);

#endif /* TCC_WASM_BACKEND_H */
