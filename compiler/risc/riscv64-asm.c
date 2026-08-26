/*************************************************************/
/*
 *  RISCV64 dummy assembler for TCC
 *
 */

#ifdef TARGET_DEFS_ONLY

#define CONFIG_TCC_ASM
#define NB_ASM_REGS 32

ST_FUNC void g(int c);
ST_FUNC void gen_le16(int c);
ST_FUNC void gen_le32(int c);

/*************************************************************/
#else
/*************************************************************/
#define USING_GLOBALS
#include "../tcc.h"

static void asm_error(void)
{
    tcc_error("RISCV64 asm not implemented.");
}

/* XXX: make it faster ? */
ST_FUNC void g(int c)
{
    int ind1;
    if (nocode_wanted)
        return;
    ind1 = ind + 1;
    if (ind1 > cur_text_section->data_allocated)
        section_realloc(cur_text_section, ind1);
    cur_text_section->data[ind] = c;
    ind = ind1;
}

ST_FUNC void gen_le16 (int i)
{
    g(i);
    g(i>>8);
}

ST_FUNC void gen_le32 (int i)
{
    gen_le16(i);
    gen_le16(i>>16);
}

ST_FUNC void gen_expr32(ExprValue *pe)
{
    gen_le32(pe->v);
}

ST_FUNC void asm_opcode(TCCState *s1, int opcode)
{
    asm_error();
}

ST_FUNC void subst_asm_operand(CString *add_str, SValue *sv, int modifier)
{
    asm_error();
}

/* generate prolog and epilog code for asm statement */
ST_FUNC void asm_gen_code(ASMOperand *operands, int nb_operands,
                         int nb_outputs, int is_output,
                         uint8_t *clobber_regs,
                         int out_reg)
{
}

ST_FUNC void asm_compute_constraints(ASMOperand *operands,
                                    int nb_operands, int nb_outputs,
                                    const uint8_t *clobber_regs,
                                    int *pout_reg)
{
}

ST_FUNC void asm_clobber(uint8_t *clobber_regs, const char *str)
{
    /* "memory" and "cc" are pseudo-clobbers -- they don't name a real
     * register, just tell the compiler not to reorder memory accesses
     * (and, on architectures with a flags register, not to reorder
     * across condition-code-setting instructions) across this asm
     * statement. This is *all* that busybox's barrier() macro
     * (`asm volatile ("":::"memory")`, no template, no operands) uses
     * inline asm for anywhere in our configured build -- confirmed by
     * checking every inline-asm call site that failed to build. It
     * needs no assembler support at all, real-register clobbers still
     * aren't supported (there is no RISC-V assembler -- see
     * docs/riscv-port-findings.md), so anything else still errors. */
    if (!strcmp(str, "memory") || !strcmp(str, "cc"))
        return;
    asm_error();
}

ST_FUNC int asm_parse_regvar (int t)
{
    asm_error();
    return -1;
}

/*************************************************************/
#endif /* ndef TARGET_DEFS_ONLY */
