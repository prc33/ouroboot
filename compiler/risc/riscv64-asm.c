/*************************************************************/
/*
 *  RISCV64 assembler for TCC
 *
 */

#ifdef TARGET_DEFS_ONLY
#define CONFIG_TCC_ASM
#define NB_ASM_REGS 64
#define ASM_DOLLAR_IN_IDENTIFIERS 1
ST_FUNC void g(int c);
ST_FUNC void gen_le16(int c);
ST_FUNC void gen_le32(int c);
#else
#define USING_GLOBALS
#include "../tcc.h"
#include "riscv64-encode.h"
enum {
    OPT_REG,
    OPT_IM12S,
    OPT_IM32,
};
#define REG_FLOAT_MASK 0x20
#define REG_IS_FLOAT(register_index) ((register_index) & REG_FLOAT_MASK)
#define REG_VALUE(register_index)    ((register_index) & (REG_FLOAT_MASK-1))
#define ENCODE_RD(register_index)  (REG_VALUE(register_index) << 7)
#define ENCODE_RS1(register_index) (REG_VALUE(register_index) << 15)
#define ENCODE_RS2(register_index) (REG_VALUE(register_index) << 20)
#define OP_IM12S (1 << OPT_IM12S)
#define OP_IM32 (1 << OPT_IM32)
#define OP_REG (1 << OPT_REG)
typedef struct Operand {
    uint32_t type;
    union {
        uint8_t reg;
        uint16_t regset;
        ExprValue e;
    };
} Operand;
enum { RVF_R, RVF_I, RVF_U, RVF_LOAD, RVF_STORE, RVF_B,
       RVF_FR, RVF_FCMP, RVF_FB, RVF_FQ, RVF_NONE, RVF_CSR, RVF_CSRI };
typedef struct { int token; unsigned char format; uint32_t bits; } RVInsn;
#define RV_INSN(name, text, format, bits) { TOK_ASM_##name, RVF_##format, bits },
static const RVInsn rv_insns[] = {
#include "riscv64-insns.h"
};
#undef RV_INSN
static const Operand zero = { OP_REG, { 0 }};
static const Operand ra = { OP_REG, { 1 }};
static const Operand zimm = { OP_IM12S };
static void asm_binary_opcode(TCCState* s1, int token);
ST_FUNC void asm_clobber(uint8_t *clobber_regs, const char *str);
ST_FUNC void asm_compute_constraints(ASMOperand *operands, int nb_operands, int nb_outputs, const uint8_t *clobber_regs, int *pout_reg);
static void asm_emit_a(int token, uint32_t opcode, const Operand *rs1, const Operand *rs2, const Operand *rd1, int aq, int rl);
static void asm_emit_b(int token, uint32_t opcode, const Operand *rs1, const Operand *rs2, const Operand *imm);
static void asm_emit_i(int token, uint32_t opcode, const Operand *rd, const Operand *rs1, const Operand *rs2);
static void asm_emit_j(int token, uint32_t opcode, const Operand *rd, const Operand *rs2);
static void asm_emit_opcode(uint32_t opcode);
static void asm_emit_r(int token, uint32_t opcode, const Operand *rd, const Operand *rs1, const Operand *rs2);
static void asm_emit_s(int token, uint32_t opcode, const Operand *rs1, const Operand *rs2, const Operand *imm);
static void asm_emit_u(int token, uint32_t opcode, const Operand *rd, const Operand *rs2);
static void asm_emit_f(int token, uint32_t opcode, const Operand *rd, const Operand *rs1, const Operand *rs2);
static void asm_emit_fb(int token, uint32_t opcode, const Operand *rd, const Operand *rs);
static void asm_emit_fq(int token, uint32_t opcode, const Operand *rd, const Operand *rs1, const Operand *rs2, const Operand *rs3);
ST_FUNC void asm_gen_code(ASMOperand *operands, int nb_operands, int nb_outputs, int is_output, uint8_t *clobber_regs, int out_reg);
static void asm_nullary_opcode(TCCState *s1, int token);
ST_FUNC void asm_opcode(TCCState *s1, int token);
static int asm_parse_csrvar(int t) {
    static const struct { int token, csr; } csrs[] = {
        { TOK_ASM_cycle, 0xc00 }, { TOK_ASM_fcsr, 3 },
        { TOK_ASM_fflags, 1 }, { TOK_ASM_frm, 2 },
        { TOK_ASM_instret, 0xc02 }, { TOK_ASM_time, 0xc01 },
        { TOK_ASM_sstatus, 0x100 }, { TOK_ASM_sie, 0x104 },
        { TOK_ASM_stvec, 0x105 }, { TOK_ASM_scounteren, 0x106 },
        { TOK_ASM_sscratch, 0x140 }, { TOK_ASM_sepc, 0x141 },
        { TOK_ASM_scause, 0x142 }, { TOK_ASM_stval, 0x143 },
        { TOK_ASM_sip, 0x144 }, { TOK_ASM_satp, 0x180 }
    };
    unsigned i;
    for (i = 0; i < sizeof(csrs) / sizeof(*csrs); ++i)
        if (csrs[i].token == t)
            return csrs[i].csr;
    return -1;
}
ST_FUNC int asm_parse_regvar(int t);
static void asm_unary_opcode(TCCState *s1, int token);
static void asm_branch_opcode(TCCState *s1, int token, int argc);
ST_FUNC void gen_expr32(ExprValue *pe);
static void parse_operand(TCCState *s1, Operand *op);
static void parse_branch_offset_operand(TCCState *s1, Operand *op);
static void parse_operands(TCCState *s1, Operand *ops, int count);
static void parse_mem_access_operands(TCCState *s1, Operand *ops);
ST_FUNC void subst_asm_operand(CString *add_str, SValue *sv, int modifier);
ST_FUNC void g(int c) {
    int next;
    if (nocode_wanted)
        return;
    next = ind + 1;
    if (next > cur_text_section->data_allocated)
        section_realloc(cur_text_section, next);
    cur_text_section->data[ind] = c;
    ind = next;
}
ST_FUNC void gen_le16(int i) { g(i); g(i >> 8); }
ST_FUNC void gen_le32(int i) { g(i); g(i >> 8); g(i >> 16); g(i >> 24); }
ST_FUNC void gen_expr32(ExprValue *pe) { gen_le32(pe->v); }
static void asm_emit_opcode(uint32_t opcode) { gen_le32(opcode); }
static void asm_nullary_opcode(TCCState *s1, int token) {
    if (token == TOK_ASM_nop)
        asm_emit_i(token, RV_addi, &zero, &zero, &zimm);
    else if (token == TOK_ASM_ret)
        asm_emit_opcode(0x67 | ENCODE_RS1(1));
    else
        expect("nullary instruction");
}
static void parse_operand(TCCState *s1, Operand *op) {
    ExprValue e = {0};
    Sym label = {0};
    int8_t reg;
    op->type = 0;
    if ((reg = asm_parse_regvar(tok)) != -1) {
        next(); // skip register name
        op->type = OP_REG;
        op->reg = (uint8_t) reg;
        return;
    } else if (tok == '$') {
        next(); // skip '#' or '$'
    } else if ((e.v = asm_parse_csrvar(tok)) != -1) {
        next();
    } else {
        asm_expr(s1, &e);
    }
    op->type = OP_IM32;
    op->e = e;
    if (!op->e.sym) {
        if ((int) op->e.v >= -0x1000 && (int) op->e.v < 0x1000)
            op->type = OP_IM12S;
    } else if (op->e.sym->type.t & (VT_EXTERN | VT_STATIC)) {
        label.type.t = VT_VOID | VT_STATIC;
        if (op->e.sym->type.t & VT_STATIC)
            greloca(cur_text_section, op->e.sym, ind, R_RISCV_PCREL_HI20, 0);
        else
            greloca(cur_text_section, op->e.sym, ind, R_RISCV_GOT_HI20, 0);
        put_extern_sym(&label, cur_text_section, ind, 0);
        greloca(cur_text_section, &label, ind+4, R_RISCV_PCREL_LO12_I, 0);
        op->type = OP_IM12S;
        op->e.v = 0;
    } else {
        expect("operand");
    }
}
static void parse_offset_operand(TCCState *s1, Operand *op, int jump) {
    ExprValue e = {0};
    asm_expr(s1, &e);
    op->type = OP_IM32;
    op->e = e;
    if (!e.sym) {
        if ((int) op->e.v >= -0x1000 && (int) op->e.v < 0x1000)
            op->type = OP_IM12S;
    } else if (e.sym->type.t & (VT_EXTERN | VT_STATIC)) {
        if (jump) {
            greloca(cur_text_section, e.sym, ind, R_RISCV_JAL, 0);
            op->type = OP_IM12S;
        }
        op->e.v = 0;
    } else
        expect("operand");
}
static void parse_branch_offset_operand(TCCState *s, Operand *op) { parse_offset_operand(s, op, 0); }
static void parse_jump_offset_operand(TCCState *s, Operand *op) { parse_offset_operand(s, op, 1); }
static void parse_operands(TCCState *s1, Operand* ops, int count){
    int i;
    for (i = 0; i < count; i++) {
        if ( i != 0 )
            skip(',');
        parse_operand(s1, &ops[i]);
    }
}
static void parse_mem_access_operands(TCCState *s1, Operand* ops){
    Operand op;
    parse_operand(s1, &ops[0]);
    skip(',');
    if ( tok == '(') {
        next();
        parse_operand(s1, &ops[1]);
        skip(')');
        ops[2] = zimm;
    } else {
        parse_operand(s1, &ops[2]);
        if ( tok == '('){
            next();
            parse_operand(s1, &ops[1]);
            skip(')');
        } else {
            op = ops[2];
            ops[1] = ops[2];
            ops[2] = op;
            ops[2] = zimm;
        }
    }
}
static void asm_jal_opcode(TCCState *s1, int token){
    Operand ops[2];
    if (token == TOK_ASM_j ){
        ops[0] = zero; // j offset
    } else if (asm_parse_regvar(tok) == -1) {
        ops[0] = ra;   // jal offset
    } else {
        parse_operand(s1, &ops[0]);
        if ( tok == ',') next(); else expect("','");
    }
    parse_jump_offset_operand(s1, &ops[1]);
    asm_emit_j(token, 0x6f, &ops[0], &ops[1]);
}
static void asm_jalr_opcode(TCCState *s1, int token){
    Operand ops[3];
    Operand op;
    parse_operand(s1, &ops[0]);
    if ( tok == ',')
        next();
    else {
        asm_emit_i(token, 0x67 | (0 << 12), &ra, &ops[0], &zimm);
        return;
    }
    if ( tok == '(') {
        next();
        parse_operand(s1, &ops[1]);
        skip(')');
        ops[2] = zimm;
    } else {
        parse_operand(s1, &ops[2]);
        if ( tok == '('){
            next();
            parse_operand(s1, &ops[1]);
            skip(')');
        } else {
            op = ops[2];
            ops[1] = ops[2];
            ops[2] = op;
            ops[2] = zimm;
        }
    }
    asm_emit_i(token, 0x67 | (0 << 12), &ops[0], &ops[1], &ops[2]);
}
static void asm_unary_opcode(TCCState *s1, int token) {
    Operand op;
    int csr = 0, r;
    parse_operand(s1, &op);
    switch (token) {
    case TOK_ASM_rdcycle: csr = 0xc00; break;
    case TOK_ASM_rdtime: csr = 0xc01; break;
    case TOK_ASM_rdinstret: csr = 0xc02; break;
    case TOK_ASM_frflags: csr = 1; break;
    case TOK_ASM_frrm: csr = 2; break;
    case TOK_ASM_frcsr: csr = 3; break;
    case TOK_ASM_jr:
        asm_emit_i(token, 0x67, &zero, &op, &zimm); return;
    case TOK_ASM_call: r = 1; goto call;
    case TOK_ASM_tail: r = 6;
      call:
        greloca(cur_text_section, op.e.sym, ind, R_RISCV_CALL, 0);
        asm_emit_opcode(RV_auipc | ENCODE_RD(r));
        asm_emit_opcode(0x67 | (token == TOK_ASM_call ? ENCODE_RD(1) : 0) |
                        ENCODE_RS1(r));
        return;
    default: expect("unary instruction");
    }
    asm_emit_opcode(RV_csrrs | ENCODE_RD(op.reg) | (csr << 20));
}
static void asm_emit_u(int token, uint32_t bits, const Operand *rd, const Operand *imm) {
    if (rd->type != OP_REG || !(imm->type & (OP_IM12S | OP_IM32)) ||
        imm->e.v >= 0x100000)
        tcc_error("invalid operands to '%s'", get_tok_str(token, NULL));
    asm_emit_opcode(bits | ENCODE_RD(rd->reg) | (imm->e.v << 12));
}
static int parse_fence_operand(){
    int t = tok;
    if ( tok == TOK_ASM_or ){
        t = TOK_ASM_or_fence;
    }
    next();
    return t - (TOK_ASM_w_fence - 1);
}
static void asm_fence_opcode(TCCState *s1, int token){
    int succ = 0xF, pred = 0xF;
    if (tok != TOK_LINEFEED && tok != ';' && tok != CH_EOF){
        pred = parse_fence_operand();
        if ( pred > 0xF || pred < 0) {
            tcc_error("'%s': Expected first operand that is a valid predecessor operand", get_tok_str(token, NULL));
        }
        skip(',');
        succ = parse_fence_operand();
        if ( succ > 0xF || succ < 0) {
            tcc_error("'%s': Expected second operand that is a valid successor operand", get_tok_str(token, NULL));
        }
    }
    asm_emit_opcode((0x3 << 2) | 3 | (0 << 12) | succ<<20 | pred<<24);
}
static void asm_binary_opcode(TCCState* s1, int token) {
    Operand imm = { OP_IM12S };
    Operand ops[2];
    int32_t lo;
    uint32_t hi;
    if (token == TOK_ASM_fscsr) {
        ops[0] = zero;
        parse_operands(s1, &ops[1], 1);
    } else {
        parse_operands(s1, &ops[0], 2);
    }
    switch (token) {
    case TOK_ASM_la:
        asm_emit_u(token, 3 | (5 << 2), ops, ops + 1);
        asm_emit_i(token, 3 | (2 << 12), ops, ops, ops + 1); return;
    case TOK_ASM_lla:
        asm_emit_u(token, 3 | (5 << 2), ops, ops + 1);
        asm_emit_i(token, 3 | (4 << 2), ops, ops, ops + 1); return;
    case TOK_ASM_li:
        if(ops[1].type != OP_IM32 && ops[1].type != OP_IM12S){
            tcc_error("'%s': Expected first source operand that is an immediate value between 0 and 0xFFFFFFFFFFFFFFFF", get_tok_str(token, NULL));
        }
        lo = ops[1].e.v;
        hi = (int64_t)ops[1].e.v >> 32;
        if(lo < 0){
            hi += 1;
        }
        imm.e.v = ((hi + 0x800) & 0xfffff000) >> 12;
        asm_emit_u(token, (0xD << 2) | 3, &ops[0], &imm);
        imm.e.v = (int32_t)hi<<20>>20;
        asm_emit_i(token, 3 | (4 << 2), &ops[0], &ops[0], &imm);
        imm.e.v = 12;
        asm_emit_i(token, (4 << 2) | 3 | (1 << 12), &ops[0], &ops[0], &imm);
        imm.e.v = (lo + (1<<19)) >> 20;
        asm_emit_i(token, 3 | (4 << 2), &ops[0], &ops[0], &imm);
        imm.e.v = 12;
        asm_emit_i(token, (4 << 2) | 3 | (1 << 12), &ops[0], &ops[0], &imm);
        lo = lo << 12 >> 12;
        imm.e.v = lo >> 8;
        asm_emit_i(token, 3 | (4 << 2), &ops[0], &ops[0], &imm);
        imm.e.v = 8;
        asm_emit_i(token, (4 << 2) | 3 | (1 << 12), &ops[0], &ops[0], &imm);
        lo &= 0xff;
        imm.e.v = lo << 20 >> 20;
        asm_emit_i(token, 3 | (4 << 2), &ops[0], &ops[0], &imm); return;
    case TOK_ASM_mv:
        asm_emit_i(token, 3 | (4 << 2), &ops[0], &ops[1], &imm); return;
    case TOK_ASM_not:
        imm.e.v = -1;
        asm_emit_i(token, (0x4 << 2) | 3 | (4 << 12), &ops[0], &ops[1], &imm); return;
    case TOK_ASM_neg:
        asm_emit_r(token, (0xC << 2) | 3 | (32 << 25), &ops[0], &zero, &ops[1]); return;
    case TOK_ASM_negw:
        asm_emit_r(token, (0xE << 2) | 3 | (32 << 25), &ops[0], &zero, &ops[1]); return;
    case TOK_ASM_sext_w:
        asm_emit_i(token, 0x1b, &ops[0], &ops[1], &zimm); return;
    case TOK_ASM_fneg_s:
        asm_emit_f(token, 0x53 | (1 << 12) | (0 << 25) | (4 << 27), &ops[0], &ops[1], &ops[1]); return;
    case TOK_ASM_fneg_d:
        asm_emit_f(token, 0x53 | (1 << 12) | (1 << 25) | (4 << 27), &ops[0], &ops[1], &ops[1]); return;
    case TOK_ASM_fmv_s:
        asm_emit_f(token, 0x53 | (0 << 12) | (0 << 25) | (4 << 27), &ops[0], &ops[1], &ops[1]); return;
    case TOK_ASM_fmv_d:
        asm_emit_f(token, 0x53 | (0 << 12) | (1 << 25) | (4 << 27), &ops[0], &ops[1], &ops[1]); return;
    case TOK_ASM_jump:
        asm_emit_opcode(3 | (5 << 2) | ENCODE_RD(5));
        greloca(cur_text_section, ops->e.sym, ind, R_RISCV_CALL, 0);
        asm_emit_opcode(0x67 | (0 << 12) | ENCODE_RS1(5)); return;
    case TOK_ASM_seqz:
        imm.e.v = 1;
        asm_emit_i(token, (0x4 << 2) | 3 | (3 << 12), &ops[0], &ops[1], &imm); return;
    case TOK_ASM_snez:
        imm.e.v = 1;
        asm_emit_r(token, (0xC << 2) | 3 | (3 << 12), &ops[0], &zero, &ops[1]); return;
    case TOK_ASM_sltz:
        asm_emit_r(token, (0xC << 2) | 3 | (2 << 12), &ops[0], &ops[1], &zero); return;
    case TOK_ASM_sgtz:
        asm_emit_r(token, (0xC << 2) | 3 | (2 << 12), &ops[0], &zero, &ops[1]); return;
    case TOK_ASM_fabs_d:
        asm_emit_f(token, 0x53 | (4 << 27) | (1 << 25) | (2 << 12), &ops[0], &ops[1], &ops[1]); return;
    case TOK_ASM_fabs_s:
        asm_emit_f(token, 0x53 | (4 << 27) | (0 << 25) | (2 << 12), &ops[0], &ops[1], &ops[1]); return;
    case TOK_ASM_csrr:
        asm_emit_opcode(0x73 | (2 << 12) | (ops[1].e.v << 20) | ENCODE_RD(ops[0].reg)); return;
    case TOK_ASM_csrw:
        asm_emit_opcode(0x73 | (1 << 12) | (ops[0].e.v << 20) | ENCODE_RS1(ops[1].reg)); return;
    case TOK_ASM_csrs:
        asm_emit_opcode(0x73 | (2 << 12) | (ops[0].e.v << 20) | ENCODE_RS1(ops[1].reg)); return;
    case TOK_ASM_csrc:
        asm_emit_opcode(0x73 | (3 << 12) | (ops[0].e.v << 20) | ENCODE_RS1(ops[1].reg)); return;
    case TOK_ASM_fsrm:
        asm_emit_opcode(0x73 | (1 << 12) | (2 << 20) | ENCODE_RD(ops[0].reg) | ENCODE_RS1(ops[1].reg)); return;
    case TOK_ASM_fscsr:
        asm_emit_opcode(0x73 | (1 << 12) | (3 << 20) | ENCODE_RD(ops[0].reg) | ENCODE_RS1(ops[1].reg)); return;
    case TOK_ASM_csrwi:
        asm_emit_opcode(0x73 | (5 << 12) | (ops[0].e.v << 20) | ENCODE_RS1(ops[1].e.v)); return;
    case TOK_ASM_csrsi:
        asm_emit_opcode(0x73 | (6 << 12) | (ops[0].e.v << 20) | ENCODE_RS1(ops[1].e.v)); return;
    case TOK_ASM_csrci:
        asm_emit_opcode(0x73 | (7 << 12) | (ops[0].e.v << 20) | ENCODE_RS1(ops[1].e.v)); return;
    default:
        expect("binary instruction");
    }
}
static void asm_emit_r(int token, uint32_t bits, const Operand *rd,
                       const Operand *rs1, const Operand *rs2) {
    if (rd->type != OP_REG || rs1->type != OP_REG || rs2->type != OP_REG)
        tcc_error("invalid operands to '%s'", get_tok_str(token, NULL));
    asm_emit_opcode(bits | ENCODE_RD(rd->reg) | ENCODE_RS1(rs1->reg) |
                    ENCODE_RS2(rs2->reg));
}
static void asm_emit_f(int token, uint32_t bits, const Operand *rd,
                       const Operand *rs1, const Operand *rs2) {
    if (rd->type != OP_REG || rs1->type != OP_REG || rs2->type != OP_REG ||
        !REG_IS_FLOAT(rd->reg) || !REG_IS_FLOAT(rs1->reg) ||
        !REG_IS_FLOAT(rs2->reg))
        tcc_error("invalid floating-point operands to '%s'", get_tok_str(token, NULL));
    asm_emit_opcode(bits | ENCODE_RD(rd->reg) | ENCODE_RS1(rs1->reg) |
                    ENCODE_RS2(rs2->reg));
}
static void asm_emit_fb(int token, uint32_t bits, const Operand *rd,
                        const Operand *rs) {
    if (rd->type != OP_REG || rs->type != OP_REG ||
        !REG_IS_FLOAT(rd->reg) || !REG_IS_FLOAT(rs->reg))
        tcc_error("invalid floating-point operands to '%s'", get_tok_str(token, NULL));
    asm_emit_opcode(bits | ENCODE_RD(rd->reg) | ENCODE_RS1(rs->reg));
}
static void asm_emit_fq(int token, uint32_t bits, const Operand *rd,
                        const Operand *rs1, const Operand *rs2, const Operand *rs3) {
    if (rd->type != OP_REG || rs1->type != OP_REG || rs2->type != OP_REG ||
        rs3->type != OP_REG || !REG_IS_FLOAT(rd->reg) ||
        !REG_IS_FLOAT(rs1->reg) || !REG_IS_FLOAT(rs2->reg) ||
        !REG_IS_FLOAT(rs3->reg))
        tcc_error("invalid floating-point operands to '%s'", get_tok_str(token, NULL));
    asm_emit_opcode(bits | ENCODE_RD(rd->reg) | ENCODE_RS1(rs1->reg) |
                    ENCODE_RS2(rs2->reg) | (REG_VALUE(rs3->reg) << 27));
}
static void asm_emit_i(int token, uint32_t bits, const Operand *rd,
                       const Operand *rs1, const Operand *imm) {
    if (rd->type != OP_REG || rs1->type != OP_REG || imm->type != OP_IM12S)
        tcc_error("invalid operands to '%s'", get_tok_str(token, NULL));
    asm_emit_opcode(bits | ENCODE_RD(rd->reg) | ENCODE_RS1(rs1->reg) |
                    (imm->e.v << 20));
}
static void asm_emit_j(int token, uint32_t opcode, const Operand* rd, const Operand* rs2) {
    uint32_t imm;
    if (rd->type != OP_REG || !(rs2->type & (OP_IM12S | OP_IM32)))
        tcc_error("invalid operands to '%s'", get_tok_str(token, NULL));
    imm = rs2->e.v;
    if ((int)imm > (1 << 20) - 1 || (int)imm < -(1 << 20) || (imm & 1))
        tcc_error("jump offset out of range or unaligned");
    gen_le32(opcode | ENCODE_RD(rd->reg) | (((imm >> 20) & 1) << 31) | (((imm >> 1) & 0x3ff) << 21) | (((imm >> 11) & 1) << 20) | (((imm >> 12) & 0xff) << 12));
}
static void asm_branch_opcode(TCCState *s1, int token, int argc) {
    static const struct { int token; uint16_t bits; char swap, zero; } tab[] = {
        { TOK_ASM_bgt, 0x4063, 1, 0 }, { TOK_ASM_ble, 0x5063, 1, 0 },
        { TOK_ASM_bgtu, 0x6063, 1, 0 }, { TOK_ASM_bleu, 0x7063, 1, 0 },
        { TOK_ASM_bnez, 0x1063, 0, 2 }, { TOK_ASM_beqz, 0x0063, 0, 2 },
        { TOK_ASM_blez, 0x5063, 1, 2 }, { TOK_ASM_bgez, 0x5063, 0, 2 },
        { TOK_ASM_bltz, 0x4063, 0, 2 }, { TOK_ASM_bgtz, 0x4063, 1, 2 }
    };
    Operand op[3];
    const Operand *a, *b;
    unsigned i;
    parse_operands(s1, op, argc - 1);
    skip(',');
    parse_branch_offset_operand(s1, op + argc - 1);
    for (i = 0; i < sizeof(tab) / sizeof(*tab) && tab[i].token != token; ++i);
    if (i == sizeof(tab) / sizeof(*tab))
        expect("branch instruction");
    a = tab[i].zero ? op : op + tab[i].swap;
    b = tab[i].zero ? &zero : op + !tab[i].swap;
    if (tab[i].zero && tab[i].swap) { a = &zero; b = op; }
    asm_emit_b(token, tab[i].bits, a, b, op + argc - 1);
}
static void asm_atomic_opcode(TCCState *s1, int token) {
    Operand op[3];
    const char *name = get_tok_str(token, NULL);
    int aq = strstr(name, ".aq") != NULL;
    int rl = strstr(name, ".rl") != NULL || strstr(name, "aqrl") != NULL;
    int funct5, is_lr = !strncmp(name, "lr.", 3);
    if (is_lr) funct5 = 2;
    else if (!strncmp(name, "sc.", 3)) funct5 = 3;
    else if (!strncmp(name, "amoadd.", 7)) funct5 = 0;
    else if (!strncmp(name, "amoswap.", 8)) funct5 = 1;
    else if (!strncmp(name, "amoxor.", 7)) funct5 = 4;
    else if (!strncmp(name, "amoor.", 6)) funct5 = 8;
    else if (!strncmp(name, "amoand.", 7)) funct5 = 12;
    else if (!strncmp(name, "amomin.", 7)) funct5 = 16;
    else if (!strncmp(name, "amomax.", 7)) funct5 = 20;
    else if (!strncmp(name, "amominu.", 8)) funct5 = 24;
    else if (!strncmp(name, "amomaxu.", 8)) funct5 = 28;
    else { expect("atomic instruction"); return; }
    parse_operand(s1, &op[0]);
    skip(',');
    if (is_lr)
        op[1] = zero;
    else {
        parse_operand(s1, &op[1]);
        skip(',');
    }
    skip('(');
    parse_operand(s1, &op[2]);
    skip(')');
    asm_emit_a(token, 0x2f | (strstr(name, ".d") ? 3 : 2) << 12
               | funct5 << 27, &op[0], &op[1], &op[2], aq, rl);
}
static void asm_emit_a(int token, uint32_t opcode, const Operand *rd1, const Operand *rs2, const Operand *rs1, int aq, int rl) {
    if (rd1->type != OP_REG || rs1->type != OP_REG || rs2->type != OP_REG)
        tcc_error("invalid operands to '%s'", get_tok_str(token, NULL));
    gen_le32(opcode | ENCODE_RS1(rs1->reg) | ENCODE_RS2(rs2->reg) | ENCODE_RD(rd1->reg) | aq << 26 | rl << 25);
}
static void asm_emit_s(int token, uint32_t bits, const Operand *rs1,
                       const Operand *rs2, const Operand *imm) {
    unsigned v;
    if (rs1->type != OP_REG || rs2->type != OP_REG || imm->type != OP_IM12S)
        tcc_error("invalid operands to '%s'", get_tok_str(token, NULL));
    v = imm->e.v;
    asm_emit_opcode(bits | ENCODE_RS1(rs1->reg) | ENCODE_RS2(rs2->reg) |
                    ((v & 0x1f) << 7) | ((v & 0xfe0) << 20));
}
static void asm_emit_b(int token, uint32_t opcode, const Operand *rs1, const Operand *rs2, const Operand *imm) {
    uint32_t offset;
    if (rs1->type != OP_REG) {
        tcc_error("'%s': Expected first source operand that is a register", get_tok_str(token, NULL));
    }
    if (rs2->type != OP_REG) {
        tcc_error("'%s': Expected destination operand that is a register", get_tok_str(token, NULL));
    }
    if (imm->type == OP_IM32 && imm->e.sym) {
        greloca(cur_text_section, imm->e.sym, ind, R_RISCV_BRANCH, 0);
        offset = 0;
    } else {
        if (imm->type != OP_IM12S)
            tcc_error("branch offset out of range");
        offset = imm->e.v;
    }
    asm_emit_opcode(opcode | ENCODE_RS1(rs1->reg) | ENCODE_RS2(rs2->reg) | (((offset >> 1) & 0xF) << 8) | (((offset >> 5) & 0x1f) << 25) | (((offset >> 11) & 1) << 7) | (((offset >> 12) & 1) << 31));
}
static int asm_fcvt_rm(TCCState *s1) {
    int rm = 7; /* dynamic */
    if (tok == ',') {
        next();
        switch (tok) {
            case TOK_ASM_rne: rm = 0; next(); break;
            case TOK_ASM_rtz: rm = 1; next(); break;
            case TOK_ASM_rdn: rm = 2; next(); break;
            case TOK_ASM_rup: rm = 3; next(); break;
            case TOK_ASM_rmm: rm = 4; next(); break;
            default: expect("rounding mode"); break;
        }
    }
    return rm;
}
static void asm_fcvt_opcode(TCCState *s1, int token) {
    Operand ops[2];
    int rm;
    uint32_t enc = 0;
    parse_operand(s1, &ops[0]);
    skip(',');
    parse_operand(s1, &ops[1]);
    switch (token) {
        case TOK_ASM_fcvt_w_s:  rm = asm_fcvt_rm(s1); enc = 0x53 | (0x60 << 25) | (rm << 12); break;
        case TOK_ASM_fcvt_wu_s: rm = asm_fcvt_rm(s1); enc = 0x53 | (0x60 << 25) | (rm << 12) | (1 << 20); break;
        case TOK_ASM_fcvt_l_s:  rm = asm_fcvt_rm(s1); enc = 0x53 | (0x60 << 25) | (rm << 12) | (2 << 20); break;
        case TOK_ASM_fcvt_lu_s: rm = asm_fcvt_rm(s1); enc = 0x53 | (0x60 << 25) | (rm << 12) | (3 << 20); break;
        case TOK_ASM_fcvt_s_w:  enc = 0x53 | (0x68 << 25) | (7 << 12); break;
        case TOK_ASM_fcvt_s_wu: enc = 0x53 | (0x68 << 25) | (7 << 12) | (1 << 20); break;
        case TOK_ASM_fcvt_s_l:  enc = 0x53 | (0x68 << 25) | (7 << 12) | (2 << 20); break;
        case TOK_ASM_fcvt_s_lu: enc = 0x53 | (0x68 << 25) | (7 << 12) | (3 << 20); break;
        case TOK_ASM_fcvt_w_d:  rm = asm_fcvt_rm(s1); enc = 0x53 | (0x61 << 25) | (rm << 12); break;
        case TOK_ASM_fcvt_wu_d: rm = asm_fcvt_rm(s1); enc = 0x53 | (0x61 << 25) | (rm << 12) | (1 << 20); break;
        case TOK_ASM_fcvt_l_d:  rm = asm_fcvt_rm(s1); enc = 0x53 | (0x61 << 25) | (rm << 12) | (2 << 20); break;
        case TOK_ASM_fcvt_lu_d: rm = asm_fcvt_rm(s1); enc = 0x53 | (0x61 << 25) | (rm << 12) | (3 << 20); break;
        case TOK_ASM_fcvt_d_w:  enc = 0x53 | (0x69 << 25) | (7 << 12); break;
        case TOK_ASM_fcvt_d_wu: enc = 0x53 | (0x69 << 25) | (7 << 12) | (1 << 20); break;
        case TOK_ASM_fcvt_d_l:  enc = 0x53 | (0x69 << 25) | (7 << 12) | (2 << 20); break;
        case TOK_ASM_fcvt_d_lu: enc = 0x53 | (0x69 << 25) | (7 << 12) | (3 << 20); break;
        case TOK_ASM_fcvt_s_d:  enc = 0x53 | (0x20 << 25) | (7 << 12) | (1 << 20); break;
        case TOK_ASM_fcvt_d_s:  enc = 0x53 | (0x21 << 25) | (7 << 12); break;
        case TOK_ASM_fclass_s:  enc = 0x53 | (0x70 << 25) | (1 << 12); break;
        case TOK_ASM_fclass_d:  enc = 0x53 | (0x71 << 25) | (1 << 12); break;
        case TOK_ASM_fmv_x_w:   enc = 0x53 | (0x70 << 25); break;
        case TOK_ASM_fmv_w_x:   enc = 0x53 | (0x78 << 25); break;
        case TOK_ASM_fmv_x_d:   enc = 0x53 | (0x71 << 25); break;
        case TOK_ASM_fmv_d_x:   enc = 0x53 | (0x79 << 25); break;
        default: expect("fcvt/fclass instruction"); return;
    }
    asm_emit_opcode(enc | ENCODE_RD(ops[0].reg) | ENCODE_RS1(ops[1].reg));
}
static int asm_real_opcode(TCCState *s1, int token) {
    const RVInsn *p;
    Operand op[4];
    int n;
    for (p = rv_insns; p < rv_insns + sizeof(rv_insns) / sizeof(*p); ++p)
        if (p->token == token)
            break;
    if (p == rv_insns + sizeof(rv_insns) / sizeof(*p))
        return 0;
    switch (p->format) {
    case RVF_NONE:
        asm_emit_opcode(p->bits);
        break;
    case RVF_LOAD:
    case RVF_STORE:
        parse_mem_access_operands(s1, op);
        if (op[1].type == OP_IM32 && op[1].e.sym &&
            (op[1].e.sym->type.t & VT_STATIC)) {
            op[1] = op[0];
            op[2].type = OP_IM12S;
            op[2].e.v = 0;
            asm_emit_u(token, RV_auipc, op, op + 2);
        }
        if (p->format == RVF_LOAD)
            asm_emit_i(token, p->bits, op, op + 1, op + 2);
        else
            asm_emit_s(token, p->bits, op + 1, op, op + 2);
        break;
    case RVF_U:
    case RVF_FB:
        parse_operands(s1, op, 2);
        if (p->format == RVF_U)
            asm_emit_u(token, p->bits, op, op + 1);
        else
            asm_emit_fb(token, p->bits, op, op + 1);
        break;
    case RVF_B:
        parse_operands(s1, op, 2);
        skip(',');
        parse_branch_offset_operand(s1, op + 2);
        asm_emit_b(token, p->bits, op, op + 1, op + 2);
        break;
    case RVF_FQ:
        parse_operands(s1, op, 4);
        asm_emit_fq(token, p->bits, op, op + 1, op + 2, op + 3);
        break;
    default:
        n = 3;
        parse_operands(s1, op, n);
        if (p->format == RVF_R)
            asm_emit_r(token, p->bits, op, op + 1, op + 2);
        else if (p->format == RVF_I)
            asm_emit_i(token, p->bits, op, op + 1, op + 2);
        else if (p->format == RVF_CSR || p->format == RVF_CSRI)
            asm_emit_opcode(p->bits | ENCODE_RD(op[0].reg) |
                            (op[1].e.v << 20) |
                            ENCODE_RS1(p->format == RVF_CSR ? op[2].reg : op[2].e.v));
        else if (p->format == RVF_FCMP)
            asm_emit_opcode(p->bits | ENCODE_RD(op[0].reg) |
                            ENCODE_RS1(op[1].reg) | ENCODE_RS2(op[2].reg));
        else
            asm_emit_f(token, p->bits, op, op + 1, op + 2);
    }
    return 1;
}
ST_FUNC void asm_opcode(TCCState *s1, int token) {
    if (asm_real_opcode(s1, token))
        return;
    if (token >= TOK_ASM_lr_w && token <= TOK_ASM_amominu_d_aqrl) {
        asm_atomic_opcode(s1, token);
        return;
    }
    switch (token) {
    case TOK_ASM_fence: asm_fence_opcode(s1, token); return;
    case TOK_ASM_sfence_vma: {
        Operand op[2];
        parse_operands(s1, op, 2);
        asm_emit_opcode(0x12000073 | ENCODE_RS1(op[0].reg) |
                         ENCODE_RS2(op[1].reg));
        return;
    }
    case TOK_ASM_jalr: asm_jalr_opcode(s1, token); return;
    case TOK_ASM_j:
    case TOK_ASM_jal: asm_jal_opcode(s1, token); return;
    case TOK_ASM_fcvt_w_s: case TOK_ASM_fcvt_wu_s:
    case TOK_ASM_fcvt_l_s: case TOK_ASM_fcvt_lu_s:
    case TOK_ASM_fcvt_s_w: case TOK_ASM_fcvt_s_wu:
    case TOK_ASM_fcvt_s_l: case TOK_ASM_fcvt_s_lu:
    case TOK_ASM_fcvt_w_d: case TOK_ASM_fcvt_wu_d:
    case TOK_ASM_fcvt_l_d: case TOK_ASM_fcvt_lu_d:
    case TOK_ASM_fcvt_d_w: case TOK_ASM_fcvt_d_wu:
    case TOK_ASM_fcvt_d_l: case TOK_ASM_fcvt_d_lu:
    case TOK_ASM_fcvt_s_d: case TOK_ASM_fcvt_d_s:
    case TOK_ASM_fclass_s: case TOK_ASM_fclass_d:
    case TOK_ASM_fmv_x_w: case TOK_ASM_fmv_w_x:
    case TOK_ASM_fmv_x_d: case TOK_ASM_fmv_d_x:
        asm_fcvt_opcode(s1, token); return;
    case TOK_ASM_nop: case TOK_ASM_ret:
        asm_nullary_opcode(s1, token); return;
    case TOK_ASM_jr: case TOK_ASM_call: case TOK_ASM_tail:
    case TOK_ASM_frflags: case TOK_ASM_frrm: case TOK_ASM_frcsr:
    case TOK_ASM_rdcycle: case TOK_ASM_rdtime: case TOK_ASM_rdinstret:
        asm_unary_opcode(s1, token); return;
    case TOK_ASM_bnez: case TOK_ASM_beqz: case TOK_ASM_blez:
    case TOK_ASM_bgez: case TOK_ASM_bltz: case TOK_ASM_bgtz:
        asm_branch_opcode(s1, token, 2); return;
    case TOK_ASM_bgt: case TOK_ASM_bgtu:
    case TOK_ASM_ble: case TOK_ASM_bleu:
        asm_branch_opcode(s1, token, 3); return;
    default:
        asm_binary_opcode(s1, token);
    }
}
ST_FUNC void subst_asm_operand(CString *add_str, SValue *sv, int modifier) {
    int r, reg, val;
    r = sv->r;
    if ((r & VT_VALMASK) == VT_CONST) {
        if (!(r & VT_LVAL) && modifier != 'c' && modifier != 'n' &&
            modifier != 'P') {
        }
        if (r & VT_SYM) {
            const char *name = get_tok_str(sv->sym->v, NULL);
            if (sv->sym->v >= SYM_FIRST_ANOM) {
                get_asm_sym(tok_alloc(name, strlen(name))->tok, sv->sym);
            }
            cstr_cat(add_str, name, -1);
            if ((uint32_t) sv->c.i == 0)
                goto no_offset;
            cstr_ccat(add_str, '+');
        }
        val = sv->c.i;
        if (modifier == 'n')
            val = -val;
        if (modifier == 'z' && val == 0) {
            cstr_cat(add_str, "zero", -1);
        } else {
            cstr_printf(add_str, "%d", val);
        }
      no_offset:;
    } else if ((r & VT_VALMASK) == VT_LOCAL) {
        cstr_printf(add_str, "%d", (int) sv->c.i);
    } else if (r & VT_LVAL) {
        reg = r & VT_VALMASK;
        if (reg >= VT_CONST)
            tcc_error("invalid inline-asm operand");
        if ((sv->type.t & VT_BTYPE) == VT_FLOAT ||
            (sv->type.t & VT_BTYPE) == VT_DOUBLE) {
            reg = TOK_ASM_f0 + REG_VALUE(reg);
        } else {
            reg = TOK_ASM_x0 + reg;
        }
        cstr_cat(add_str, get_tok_str(reg, NULL), -1);
    } else {
        reg = r & VT_VALMASK;
        if (reg >= VT_CONST)
            tcc_error("invalid inline-asm operand");
        if ((sv->type.t & VT_BTYPE) == VT_FLOAT ||
            (sv->type.t & VT_BTYPE) == VT_DOUBLE) {
            reg = TOK_ASM_f0 + REG_VALUE(reg);
        } else {
            reg = TOK_ASM_x0 + reg;
        }
        cstr_cat(add_str, get_tok_str(reg, NULL), -1);
    }
}
static int tcc_ireg(int r) { return REG_VALUE(r) - 10; }
static int tcc_freg(int r) { return REG_VALUE(r) - 2; }
ST_FUNC void asm_gen_code(ASMOperand *operands, int nb_operands,
                         int nb_outputs, int is_output,
                         uint8_t *clobber_regs,
                         int out_reg) {
    uint8_t regs_allocated[NB_ASM_REGS];
    ASMOperand *op;
    int i, reg;
    static const uint8_t reg_saved[] = {
        8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
        40, 41, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59
    };
    memcpy(regs_allocated, clobber_regs, sizeof(regs_allocated));
    for(i = 0; i < nb_operands; i++) {
        op = &operands[i];
        if (op->reg >= 0) {
            regs_allocated[op->reg] = 1;
        }
    }
    if(!is_output) {
        for(i = 0; i < sizeof(reg_saved)/sizeof(reg_saved[0]); i++) {
            reg = reg_saved[i];
            if (regs_allocated[reg]) {
                gen_le32((4 << 2) | 3 |
                        ENCODE_RD(2) | ENCODE_RS1(2) | (unsigned)-8 << 20);
                if (REG_IS_FLOAT(reg)){
                    gen_le32( 0x27 | (3 << 12) |
                            ENCODE_RS2(reg) | ENCODE_RS1(2) );
                } else {
                    gen_le32((0x8 << 2) | 3 | (3 << 12) |
                            ENCODE_RS2(reg) | ENCODE_RS1(2) );
                }
            }
        }
        for(i = 0; i < nb_operands; i++) {
            op = &operands[i];
            if (op->reg >= 0) {
                if ((op->vt->r & VT_VALMASK) == VT_LLOCAL &&
                    op->is_memory) {
                    SValue sv;
                    sv = *op->vt;
                    sv.r = (sv.r & ~VT_VALMASK) | VT_LOCAL | VT_LVAL;
                    sv.type.t = VT_PTR;
                    load(tcc_ireg(op->reg), &sv);
                } else if (i >= nb_outputs || op->is_rw) {
                    if ((op->vt->type.t & VT_BTYPE) == VT_FLOAT ||
                        (op->vt->type.t & VT_BTYPE) == VT_DOUBLE) {
                        load(tcc_freg(op->reg), op->vt);
                    } else {
                        load(tcc_ireg(op->reg), op->vt);
                    }
                }
            }
        }
    } else {
        for(i = 0 ; i < nb_outputs; i++) {
            op = &operands[i];
            if (op->reg >= 0) {
                if ((op->vt->r & VT_VALMASK) == VT_LLOCAL) {
                    if (!op->is_memory) {
                        SValue sv;
                        sv = *op->vt;
                        sv.r = (sv.r & ~VT_VALMASK) | VT_LOCAL;
                        sv.type.t = VT_PTR;
                        load(tcc_ireg(out_reg), &sv);
                        sv = *op->vt;
                        sv.r = (sv.r & ~VT_VALMASK) | out_reg;
                        store(tcc_ireg(op->reg), &sv);
                    }
                } else {
                    if ((op->vt->type.t & VT_BTYPE) == VT_FLOAT ||
                        (op->vt->type.t & VT_BTYPE) == VT_DOUBLE) {
                        store(tcc_freg(op->reg), op->vt);
                    } else {
                        store(tcc_ireg(op->reg), op->vt);
                    }
                }
            }
        }
        for(i = sizeof(reg_saved)/sizeof(reg_saved[0]) - 1; i >= 0; i--) {
            reg = reg_saved[i];
            if (regs_allocated[reg]) {
                if (REG_IS_FLOAT(reg)){
                    gen_le32(7 | (3 << 12) |
                            ENCODE_RD(reg) | ENCODE_RS1(2) | 0);
                } else {
                    gen_le32(3 | (3 << 12) |
                            ENCODE_RD(reg) | ENCODE_RS1(2) | 0);
                }
                gen_le32((4 << 2) | 3 |
                        ENCODE_RD(2) | ENCODE_RS1(2) | 8 << 20);
            }
        }
    }
}
static inline int constraint_priority(const char *str) {
    int priority, c, pr;
    priority = 0;
    for(;;) {
        c = *str;
        if (c == '\0')
            break;
        str++;
        switch(c) {
        case 'A': // address that is held in a general-purpose register.
        case 'S': // constraint that matches an absolute symbolic address.
        case 'f': // register [float]
        case 'r': // register [general]
        case 'p': // valid memory address for load,store [general]
            pr = 3;
            break;
        case 'I': // 12 bit signed immedate
        case 'i': // immediate integer operand, including symbolic constants [general]
        case 'm': // memory operand [general]
        case 'g': // general-purpose-register, memory, immediate integer [general]
            pr = 4;
            break;
        case 'v':
            tcc_error("unimp: constraint '%c'", c);
        default:
            tcc_error("unknown constraint '%d'", c);
        }
        if (pr > priority)
            priority = pr;
    }
    return priority;
}
static const char *skip_constraint_modifiers(const char *p) {
    while (*p == '=' || *p == '&' || *p == '+' || *p == '%')
        p++;
    return p;
}
#define REG_OUT_MASK 0x01
#define REG_IN_MASK  0x02
#define is_reg_allocated(reg) (regs_allocated[reg] & reg_mask)
ST_FUNC void asm_compute_constraints(ASMOperand *operands,
                                    int nb_operands, int nb_outputs,
                                    const uint8_t *clobber_regs,
                                    int *pout_reg) {
    ASMOperand *op;
    int sorted_op[MAX_ASM_OPERANDS];
    int i, j, k, p1, p2, tmp, reg, c, reg_mask;
    const char *str;
    uint8_t regs_allocated[NB_ASM_REGS];
    for (i = 0; i < nb_operands; i++) {
        op = &operands[i];
        op->input_index = -1;
        op->ref_index = -1;
        op->reg = -1;
        op->is_memory = 0;
        op->is_rw = 0;
    }
    for (i = 0; i < nb_operands; i++) {
        op = &operands[i];
        str = op->constraint;
        str = skip_constraint_modifiers(str);
        if (isnum(*str) || *str == '[') {
            k = find_constraint(operands, nb_operands, str, NULL);
            if ((unsigned) k >= i || i < nb_outputs)
                tcc_error("invalid reference in constraint %d ('%s')",
                          i, str);
            op->ref_index = k;
            if (operands[k].input_index >= 0)
                tcc_error("cannot reference twice the same operand");
            operands[k].input_index = i;
            op->priority = 5;
        } else if ((op->vt->r & VT_VALMASK) == VT_LOCAL
                   && op->vt->sym
                   && (reg = op->vt->sym->r & VT_VALMASK) < VT_CONST) {
            op->priority = 1;
            op->reg = reg;
        } else {
            op->priority = constraint_priority(str);
        }
    }
    for (i = 0; i < nb_operands; i++)
        sorted_op[i] = i;
    for (i = 0; i < nb_operands - 1; i++) {
        for (j = i + 1; j < nb_operands; j++) {
            p1 = operands[sorted_op[i]].priority;
            p2 = operands[sorted_op[j]].priority;
            if (p2 < p1) {
                tmp = sorted_op[i];
                sorted_op[i] = sorted_op[j];
                sorted_op[j] = tmp;
            }
        }
    }
    for (i = 0; i < NB_ASM_REGS; i++) {
        if (clobber_regs[i])
            regs_allocated[i] = REG_IN_MASK | REG_OUT_MASK;
        else
            regs_allocated[i] = 0;
    }
    for (i = 0; i < nb_operands; i++) {
        j = sorted_op[i];
        op = &operands[j];
        str = op->constraint;
        if (op->ref_index >= 0)
            continue;
        if (op->input_index >= 0) {
            reg_mask = REG_IN_MASK | REG_OUT_MASK;
        } else if (j < nb_outputs) {
            reg_mask = REG_OUT_MASK;
        } else {
            reg_mask = REG_IN_MASK;
        }
        if (op->reg >= 0) {
            if (is_reg_allocated(op->reg))
                tcc_error
                    ("asm regvar requests register that's taken already");
            reg = op->reg;
        }
      try_next:
        c = *str++;
        switch (c) {
        case '=': // Operand is written-to
            goto try_next;
        case '+': // Operand is both READ and written-to
            op->is_rw = 1;
        case '&': // Operand is clobbered before the instruction is done using the input operands
            if (j >= nb_outputs)
                tcc_error("'%c' modifier can only be applied to outputs", c);
            reg_mask = REG_IN_MASK | REG_OUT_MASK;
            goto try_next;
        case 'r': // general-purpose register
        case 'p': // loadable/storable address
            if ((reg = op->reg) >= 0)
                goto reg_found;
            else for (reg = 10; reg <= 18; reg++) {
                if (!is_reg_allocated(reg))
                    goto reg_found;
            }
            goto try_next;
          reg_found:
            op->is_llong = 0;
            op->reg = reg;
            regs_allocated[reg] |= reg_mask;
            break;
        case 'f': // floating pont register
            if ((reg = op->reg) >= 0)
                goto reg_found;
            else for (reg = 42; reg <= 50; reg++) {
                if (!is_reg_allocated(reg))
                    goto reg_found;
            }
            goto try_next;
        case 'I': // I-Type 12 bit signed immediate
        case 'i': // immediate integer operand, including symbolic constants
            if (!((op->vt->r & (VT_VALMASK | VT_LVAL)) == VT_CONST))
                goto try_next;
            break;
        case 'm': // memory operand
        case 'g': // any register
            if (j < nb_outputs || c == 'm') {
                if ((op->vt->r & VT_VALMASK) == VT_LLOCAL) {
                    for (reg = 10; reg <= 18; reg++) {
                        if (!(regs_allocated[reg] & REG_IN_MASK))
                            goto reg_found1;
                    }
                    goto try_next;
                  reg_found1:
                    regs_allocated[reg] |= REG_IN_MASK;
                    op->reg = reg;
                    op->is_memory = 1;
                }
            }
            break;
        default:
            tcc_error("asm constraint %d ('%s') could not be satisfied",
                      j, op->constraint);
            break;
        }
        if (op->input_index >= 0) {
            operands[op->input_index].reg = op->reg;
            operands[op->input_index].is_llong = op->is_llong;
        }
    }
    *pout_reg = -1;
    for (i = 0; i < nb_operands; i++) {
        op = &operands[i];
        if (op->reg >= 0 &&
            (op->vt->r & VT_VALMASK) == VT_LLOCAL && !op->is_memory) {
            if (REG_IS_FLOAT(op->reg)){
                for (reg = 42; reg <= 50; reg++) {
                    if (!(regs_allocated[reg] & REG_OUT_MASK))
                        goto reg_found2;
                }
            } else {
                for (reg = 10; reg <= 18; reg++) {
                    if (!(regs_allocated[reg] & REG_OUT_MASK))
                        goto reg_found2;
                }
            }
            tcc_error("could not find free output register for reloading");
          reg_found2:
            *pout_reg = reg;
            break;
        }
    }
}
ST_FUNC void asm_clobber(uint8_t *clobber_regs, const char *str) {
    int reg;
    TokenSym *ts;
    if (!strcmp(str, "memory") ||
        !strcmp(str, "cc") ||
        !strcmp(str, "flags"))
        return;
    ts = tok_alloc(str, strlen(str));
    reg = asm_parse_regvar(ts->tok);
    if (reg == -1) {
        tcc_error("invalid clobber register '%s'", str);
    }
    clobber_regs[reg] = 1;
}
ST_FUNC int asm_parse_regvar (int t) {
    if (t >= TOK_ASM_pc || t < TOK_ASM_x0)
        return -1;
    if (t < TOK_ASM_f0)
        return t - TOK_ASM_x0;
    if (t < TOK_ASM_zero)
        return t - TOK_ASM_f0 + 32; // Use higher 32 for floating point
    if (t < TOK_ASM_ft0)
        return t - TOK_ASM_zero;
    return t - TOK_ASM_ft0 + 32; // Use higher 32 for floating point
}
#endif /* ndef TARGET_DEFS_ONLY */
