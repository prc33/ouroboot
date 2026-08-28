/* ------------------------------------------------------------------ */
/* WARNING: relative order of tokens is important.                    */

/*
 * The specifications are available under https://riscv.org/technical/specifications/
 */

#define DEF_ASM_WITH_SUFFIX(x, y) \
  DEF(TOK_ASM_ ## x ## _ ## y, #x "." #y)

#define DEF_ASM_WITH_SUFFIXES(x, y, z) \
  DEF(TOK_ASM_ ## x ## _ ## y ## _ ## z, #x "." #y "." #z)

#define DEF_ASM_FENCE(x) \
  DEF(TOK_ASM_ ## x ## _fence, #x)

/* register */
 /* integer */
 DEF_ASM(x0)
 DEF_ASM(x1)
 DEF_ASM(x2)
 DEF_ASM(x3)
 DEF_ASM(x4)
 DEF_ASM(x5)
 DEF_ASM(x6)
 DEF_ASM(x7)
 DEF_ASM(x8)
 DEF_ASM(x9)
 DEF_ASM(x10)
 DEF_ASM(x11)
 DEF_ASM(x12)
 DEF_ASM(x13)
 DEF_ASM(x14)
 DEF_ASM(x15)
 DEF_ASM(x16)
 DEF_ASM(x17)
 DEF_ASM(x18)
 DEF_ASM(x19)
 DEF_ASM(x20)
 DEF_ASM(x21)
 DEF_ASM(x22)
 DEF_ASM(x23)
 DEF_ASM(x24)
 DEF_ASM(x25)
 DEF_ASM(x26)
 DEF_ASM(x27)
 DEF_ASM(x28)
 DEF_ASM(x29)
 DEF_ASM(x30)
 DEF_ASM(x31)
 /* float */
 DEF_ASM(f0)
 DEF_ASM(f1)
 DEF_ASM(f2)
 DEF_ASM(f3)
 DEF_ASM(f4)
 DEF_ASM(f5)
 DEF_ASM(f6)
 DEF_ASM(f7)
 DEF_ASM(f8)
 DEF_ASM(f9)
 DEF_ASM(f10)
 DEF_ASM(f11)
 DEF_ASM(f12)
 DEF_ASM(f13)
 DEF_ASM(f14)
 DEF_ASM(f15)
 DEF_ASM(f16)
 DEF_ASM(f17)
 DEF_ASM(f18)
 DEF_ASM(f19)
 DEF_ASM(f20)
 DEF_ASM(f21)
 DEF_ASM(f22)
 DEF_ASM(f23)
 DEF_ASM(f24)
 DEF_ASM(f25)
 DEF_ASM(f26)
 DEF_ASM(f27)
 DEF_ASM(f28)
 DEF_ASM(f29)
 DEF_ASM(f30)
 DEF_ASM(f31)

/* register ABI mnemonics, refer to RISC-V ABI 1.0 */
 /* integer */
 DEF_ASM(zero)
 DEF_ASM(ra)
 DEF_ASM(sp)
 DEF_ASM(gp)
 DEF_ASM(tp)
 DEF_ASM(t0)
 DEF_ASM(t1)
 DEF_ASM(t2)
 DEF_ASM(s0)
 DEF_ASM(s1)
 DEF_ASM(a0)
 DEF_ASM(a1)
 DEF_ASM(a2)
 DEF_ASM(a3)
 DEF_ASM(a4)
 DEF_ASM(a5)
 DEF_ASM(a6)
 DEF_ASM(a7)
 DEF_ASM(s2)
 DEF_ASM(s3)
 DEF_ASM(s4)
 DEF_ASM(s5)
 DEF_ASM(s6)
 DEF_ASM(s7)
 DEF_ASM(s8)
 DEF_ASM(s9)
 DEF_ASM(s10)
 DEF_ASM(s11)
 DEF_ASM(t3)
 DEF_ASM(t4)
 DEF_ASM(t5)
 DEF_ASM(t6)
 /* float */
 DEF_ASM(ft0)
 DEF_ASM(ft1)
 DEF_ASM(ft2)
 DEF_ASM(ft3)
 DEF_ASM(ft4)
 DEF_ASM(ft5)
 DEF_ASM(ft6)
 DEF_ASM(ft7)
 DEF_ASM(fs0)
 DEF_ASM(fs1)
 DEF_ASM(fa0)
 DEF_ASM(fa1)
 DEF_ASM(fa2)
 DEF_ASM(fa3)
 DEF_ASM(fa4)
 DEF_ASM(fa5)
 DEF_ASM(fa6)
 DEF_ASM(fa7)
 DEF_ASM(fs2)
 DEF_ASM(fs3)
 DEF_ASM(fs4)
 DEF_ASM(fs5)
 DEF_ASM(fs6)
 DEF_ASM(fs7)
 DEF_ASM(fs8)
 DEF_ASM(fs9)
 DEF_ASM(fs10)
 DEF_ASM(fs11)
 DEF_ASM(ft8)
 DEF_ASM(ft9)
 DEF_ASM(ft10)
 DEF_ASM(ft11)
 /* not in the ABI */
 DEF_ASM(pc)

/*   Loads */

 DEF_ASM(lb)
 DEF_ASM(lh)
 DEF_ASM(lw)
 DEF_ASM(lbu)
 DEF_ASM(lhu)
 /* RV64 */
 DEF_ASM(ld)
 DEF_ASM(lwu)

/* Stores */

 DEF_ASM(sb)
 DEF_ASM(sh)
 DEF_ASM(sw)
 /* RV64 */
 DEF_ASM(sd)

/* Shifts */

 DEF_ASM(sll)
 DEF_ASM(srl)
 DEF_ASM(sra)
 /* RV64 */
 DEF_ASM(slli)
 DEF_ASM(srli)
 DEF_ASM(sllw)
 DEF_ASM(slliw)
 DEF_ASM(srlw)
 DEF_ASM(srliw)
 DEF_ASM(srai)
 DEF_ASM(sraw)
 DEF_ASM(sraiw)

/* Arithmetic */

 DEF_ASM(add)
 DEF_ASM(addi)
 DEF_ASM(sub)
 DEF_ASM(lui)
 DEF_ASM(auipc)
 /* RV64 */
 DEF_ASM(addw)
 DEF_ASM(addiw)
 DEF_ASM(subw)

/* Logical */

 DEF_ASM(xor)
 DEF_ASM(xori)
 DEF_ASM(or)
 DEF_ASM(ori)
 DEF_ASM(and)
 DEF_ASM(andi)

/* Compare */

 DEF_ASM(slt)
 DEF_ASM(slti)
 DEF_ASM(sltu)
 DEF_ASM(sltiu)

/* Branch */

 DEF_ASM(beq)
 DEF_ASM(bne)
 DEF_ASM(blt)
 DEF_ASM(bge)
 DEF_ASM(bltu)
 DEF_ASM(bgeu)

/* Jump */

 DEF_ASM(jal)
 DEF_ASM(jalr)

/* Sync */

 DEF_ASM(fence)
 /* Zifencei extension */
 DEF_ASM_WITH_SUFFIX(fence, i)

/* System call */

 /* used to be called scall and sbreak */
 DEF_ASM(ecall)
 DEF_ASM(ebreak)

/* Counters */

 DEF_ASM(rdcycle)
 DEF_ASM(rdtime)
 DEF_ASM(rdinstret)

/* “M” Standard Extension for Integer Multiplication and Division, V2.0 */
 DEF_ASM(mul)
 DEF_ASM(mulh)
 DEF_ASM(mulhsu)
 DEF_ASM(mulhu)
 DEF_ASM(div)
 DEF_ASM(divu)
 DEF_ASM(rem)
 DEF_ASM(remu)
 /* RV64 */
 DEF_ASM(mulw)
 DEF_ASM(divw)
 DEF_ASM(divuw)
 DEF_ASM(remw)
 DEF_ASM(remuw)

/* "F"/"D" Extension for Single/Double-Precision Floating Point Arithmetic, V2.2 */
 /* enough implemented for musl */
 DEF_ASM_WITH_SUFFIX(fsgnj, s)
 DEF_ASM_WITH_SUFFIX(fsgnj, d)
 DEF_ASM_WITH_SUFFIX(fsgnjn, s)
 DEF_ASM_WITH_SUFFIX(fsgnjn, d)
 DEF_ASM_WITH_SUFFIX(fsgnjx, s)
 DEF_ASM_WITH_SUFFIX(fsgnjx, d)
 DEF_ASM_WITH_SUFFIX(fadd, s)
 DEF_ASM_WITH_SUFFIX(fadd, d)
 DEF_ASM_WITH_SUFFIX(fsub, s)
 DEF_ASM_WITH_SUFFIX(fsub, d)
 DEF_ASM_WITH_SUFFIX(fmul, s)
 DEF_ASM_WITH_SUFFIX(fmul, d)
 DEF_ASM_WITH_SUFFIX(fdiv, s)
 DEF_ASM_WITH_SUFFIX(fdiv, d)
 DEF_ASM_WITH_SUFFIX(fmadd, s)
 DEF_ASM_WITH_SUFFIX(fmadd, d)
 DEF_ASM_WITH_SUFFIX(fmsub, s)
 DEF_ASM_WITH_SUFFIX(fmsub, d)
 DEF_ASM_WITH_SUFFIX(fnmsub, s)
 DEF_ASM_WITH_SUFFIX(fnmsub, d)
 DEF_ASM_WITH_SUFFIX(fnmadd, s)
 DEF_ASM_WITH_SUFFIX(fnmadd, d)
 DEF_ASM_WITH_SUFFIX(fmax, s)
 DEF_ASM_WITH_SUFFIX(fmax, d)
 DEF_ASM_WITH_SUFFIX(fmin, s)
 DEF_ASM_WITH_SUFFIX(fmin, d)
 DEF_ASM_WITH_SUFFIX(fsqrt, s)
 DEF_ASM_WITH_SUFFIX(fsqrt, d)

 /* F/D comparison and conversion (not needed by musl, added for completeness) */
 DEF_ASM_WITH_SUFFIX(feq, s)
 DEF_ASM_WITH_SUFFIX(feq, d)
 DEF_ASM_WITH_SUFFIX(flt, s)
 DEF_ASM_WITH_SUFFIX(flt, d)
 DEF_ASM_WITH_SUFFIX(fle, s)
 DEF_ASM_WITH_SUFFIX(fle, d)
 DEF_ASM_WITH_SUFFIX(fclass, s)
 DEF_ASM_WITH_SUFFIX(fclass, d)
 DEF_ASM_WITH_SUFFIXES(fcvt, w, s)
 DEF_ASM_WITH_SUFFIXES(fcvt, wu, s)
 DEF_ASM_WITH_SUFFIXES(fcvt, l, s)
 DEF_ASM_WITH_SUFFIXES(fcvt, lu, s)
 DEF_ASM_WITH_SUFFIXES(fcvt, s, w)
 DEF_ASM_WITH_SUFFIXES(fcvt, s, wu)
 DEF_ASM_WITH_SUFFIXES(fcvt, s, l)
 DEF_ASM_WITH_SUFFIXES(fcvt, s, lu)
 DEF_ASM_WITH_SUFFIXES(fcvt, w, d)
 DEF_ASM_WITH_SUFFIXES(fcvt, wu, d)
 DEF_ASM_WITH_SUFFIXES(fcvt, l, d)
 DEF_ASM_WITH_SUFFIXES(fcvt, lu, d)
 DEF_ASM_WITH_SUFFIXES(fcvt, d, w)
 DEF_ASM_WITH_SUFFIXES(fcvt, d, wu)
 DEF_ASM_WITH_SUFFIXES(fcvt, d, l)
 DEF_ASM_WITH_SUFFIXES(fcvt, d, lu)
 DEF_ASM_WITH_SUFFIXES(fcvt, s, d)
 DEF_ASM_WITH_SUFFIXES(fcvt, d, s)
 DEF_ASM_WITH_SUFFIXES(fmv, x, w)
 DEF_ASM_WITH_SUFFIXES(fmv, w, x)
 DEF_ASM_WITH_SUFFIXES(fmv, x, d)
 DEF_ASM_WITH_SUFFIXES(fmv, d, x)

/* “Zicsr”, Control and Status Register (CSR) Instructions, V2.0 */
 DEF_ASM(csrrw)
 DEF_ASM(csrrs)
 DEF_ASM(csrrc)
 DEF_ASM(csrrwi)
 DEF_ASM(csrrsi)
 DEF_ASM(csrrci)
 /* registers */
 DEF_ASM(cycle)
 DEF_ASM(fcsr)
 DEF_ASM(fflags)
 DEF_ASM(frm)
 DEF_ASM(instret)
 DEF_ASM(time)
 DEF_ASM(sstatus)
 DEF_ASM(sie)
 DEF_ASM(stvec)
 DEF_ASM(scounteren)
 DEF_ASM(sscratch)
 DEF_ASM(sepc)
 DEF_ASM(scause)
 DEF_ASM(stval)
 DEF_ASM(sip)
 DEF_ASM(satp)
 /* pseudo */
 DEF_ASM(csrc)
 DEF_ASM(csrci)
 DEF_ASM(csrr)
 DEF_ASM(csrs)
 DEF_ASM(csrsi)
 DEF_ASM(csrw)
 DEF_ASM(csrwi)
 DEF_ASM(frcsr)
 DEF_ASM(frflags)
 DEF_ASM(frrm)
 DEF_ASM(fscsr)
 DEF_ASM(fsflags)
 DEF_ASM(fsrm)

/* Supervisor instructions used by the kernel. */
 DEF_ASM_WITH_SUFFIX(sfence, vma)
 DEF_ASM(sret)
 DEF_ASM(wfi)

/* pseudoinstructions */
 DEF_ASM(beqz)
 DEF_ASM(bgez)
 DEF_ASM(bgt)
 DEF_ASM(bgtu)
 DEF_ASM(bgtz)
 DEF_ASM(ble)
 DEF_ASM(bleu)
 DEF_ASM(blez)
 DEF_ASM(bltz)
 DEF_ASM(bnez)
 DEF_ASM(call)
 DEF_ASM_WITH_SUFFIX(fabs, d)
 DEF_ASM_WITH_SUFFIX(fabs, s)
 DEF_ASM(fld)
 DEF_ASM(flw)
 DEF_ASM_WITH_SUFFIX(fmv, d)
 DEF_ASM_WITH_SUFFIX(fmv, s)
 DEF_ASM_WITH_SUFFIX(fneg, d)
 DEF_ASM_WITH_SUFFIX(fneg, s)
 DEF_ASM(fsd)
 DEF_ASM(fsw)
 DEF_ASM(j)
 DEF_ASM(jump)
 DEF_ASM(jr)
 DEF_ASM(la)
 DEF_ASM(li)
 DEF_ASM(lla)
 DEF_ASM(mv)
 DEF_ASM(neg)
 DEF_ASM(negw)
 DEF_ASM(nop)
 DEF_ASM(not)
 DEF_ASM(ret)
 DEF_ASM(seqz)
 DEF_ASM_WITH_SUFFIX(sext, w)
 DEF_ASM(sgtz)
 DEF_ASM(sltz)
 DEF_ASM(snez)
 DEF_ASM(tail)

/* Possible values for .option directive */
 DEF_ASM(arch)
 DEF_ASM(rvc)
 DEF_ASM(norvc)
 DEF_ASM(pic)
 DEF_ASM(nopic)
 DEF_ASM(relax)
 DEF_ASM(norelax)

/* “A” Standard Extension for Atomic Instructions, Version 2.1 */
#define DEF_ATOMIC_WIDTH(x, w) \
 DEF_ASM_WITH_SUFFIX(x, w) \
 DEF_ASM_WITH_SUFFIXES(x, w, aq) \
 DEF_ASM_WITH_SUFFIXES(x, w, rl) \
 DEF_ASM_WITH_SUFFIXES(x, w, aqrl)
#define DEF_ATOMIC(x) DEF_ATOMIC_WIDTH(x, w) DEF_ATOMIC_WIDTH(x, d)
 DEF_ATOMIC(lr)
 DEF_ATOMIC(sc)
 DEF_ATOMIC(amoadd)
 DEF_ATOMIC(amoswap)
 DEF_ATOMIC(amoand)
 DEF_ATOMIC(amoor)
 DEF_ATOMIC(amoxor)
 DEF_ATOMIC(amomax)
 DEF_ATOMIC(amomaxu)
 DEF_ATOMIC(amomin)
 DEF_ATOMIC(amominu)
#undef DEF_ATOMIC
#undef DEF_ATOMIC_WIDTH

 /* rounding mode keywords (used as fcvt operand: fcvt.w.s rd, rs1, rtz) */
 DEF_ASM(rne)
 DEF_ASM(rtz)
 DEF_ASM(rdn)
 DEF_ASM(rup)
 DEF_ASM(rmm)

 /* `fence` arguments */
/* NOTE: Order is important */
 DEF_ASM_FENCE(w)
 DEF_ASM_FENCE(r)
 DEF_ASM_FENCE(rw)

 DEF_ASM_FENCE(o)
 DEF_ASM_FENCE(ow)
 DEF_ASM_FENCE(or)
 DEF_ASM_FENCE(orw)

 DEF_ASM_FENCE(i)
 DEF_ASM_FENCE(iw)
 DEF_ASM_FENCE(ir)
 DEF_ASM_FENCE(irw)

 DEF_ASM_FENCE(io)
 DEF_ASM_FENCE(iow)
 DEF_ASM_FENCE(ior)
 DEF_ASM_FENCE(iorw)

#undef DEF_ASM_FENCE
#undef DEF_ASM_WITH_SUFFIX
#undef DEF_ASM_WITH_SUFFIXES
