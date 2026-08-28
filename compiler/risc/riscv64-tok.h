#define DEF_ASM_WITH_SUFFIX(x, y) \
  DEF(TOK_ASM_ ## x ## _ ## y, #x "." #y)
#define DEF_ASM_WITH_SUFFIXES(x, y, z) \
  DEF(TOK_ASM_ ## x ## _ ## y ## _ ## z, #x "." #y "." #z)
#define DEF_ASM_FENCE(x) \
  DEF(TOK_ASM_ ## x ## _fence, #x)
#define DEF_NUM32(p) \
 DEF(TOK_ASM_ ## p ## 0, #p "0") DEF(TOK_ASM_ ## p ## 1, #p "1")  DEF(TOK_ASM_ ## p ## 2, #p "2") DEF(TOK_ASM_ ## p ## 3, #p "3") \
 DEF(TOK_ASM_ ## p ## 4, #p "4") DEF(TOK_ASM_ ## p ## 5, #p "5")  DEF(TOK_ASM_ ## p ## 6, #p "6") DEF(TOK_ASM_ ## p ## 7, #p "7") \
 DEF(TOK_ASM_ ## p ## 8, #p "8") DEF(TOK_ASM_ ## p ## 9, #p "9")  DEF(TOK_ASM_ ## p ## 10, #p "10") DEF(TOK_ASM_ ## p ## 11, #p "11") \
 DEF(TOK_ASM_ ## p ## 12, #p "12") DEF(TOK_ASM_ ## p ## 13, #p "13")  DEF(TOK_ASM_ ## p ## 14, #p "14") DEF(TOK_ASM_ ## p ## 15, #p "15") \
 DEF(TOK_ASM_ ## p ## 16, #p "16") DEF(TOK_ASM_ ## p ## 17, #p "17")  DEF(TOK_ASM_ ## p ## 18, #p "18") DEF(TOK_ASM_ ## p ## 19, #p "19") \
 DEF(TOK_ASM_ ## p ## 20, #p "20") DEF(TOK_ASM_ ## p ## 21, #p "21")  DEF(TOK_ASM_ ## p ## 22, #p "22") DEF(TOK_ASM_ ## p ## 23, #p "23") \
 DEF(TOK_ASM_ ## p ## 24, #p "24") DEF(TOK_ASM_ ## p ## 25, #p "25")  DEF(TOK_ASM_ ## p ## 26, #p "26") DEF(TOK_ASM_ ## p ## 27, #p "27") \
 DEF(TOK_ASM_ ## p ## 28, #p "28") DEF(TOK_ASM_ ## p ## 29, #p "29")  DEF(TOK_ASM_ ## p ## 30, #p "30") DEF(TOK_ASM_ ## p ## 31, #p "31")
#define DEF_REG8(a,b,c,d,e,f,g,h) \
 DEF_ASM(a) DEF_ASM(b) DEF_ASM(c) DEF_ASM(d) DEF_ASM(e) DEF_ASM(f) DEF_ASM(g) DEF_ASM(h)
DEF_NUM32(x)
DEF_NUM32(f)
DEF_REG8(zero,ra,sp,gp,tp,t0,t1,t2)
DEF_REG8(s0,s1,a0,a1,a2,a3,a4,a5)
DEF_REG8(a6,a7,s2,s3,s4,s5,s6,s7)
DEF_REG8(s8,s9,s10,s11,t3,t4,t5,t6)
DEF_REG8(ft0,ft1,ft2,ft3,ft4,ft5,ft6,ft7)
DEF_REG8(fs0,fs1,fa0,fa1,fa2,fa3,fa4,fa5)
DEF_REG8(fa6,fa7,fs2,fs3,fs4,fs5,fs6,fs7)
DEF_REG8(fs8,fs9,fs10,fs11,ft8,ft9,ft10,ft11)
#undef DEF_REG8
#undef DEF_NUM32
 DEF_ASM(pc)
#define RV_INSN(name, spelling, format, bits) DEF(TOK_ASM_ ## name, spelling)
#include "riscv64-insns.h"
#undef RV_INSN
 DEF_ASM(jal) DEF_ASM(jalr) DEF_ASM(fence)
 DEF_ASM(rdcycle) DEF_ASM(rdtime) DEF_ASM(rdinstret)
 DEF_ASM_WITH_SUFFIX(fclass, s) DEF_ASM_WITH_SUFFIX(fclass, d) DEF_ASM_WITH_SUFFIXES(fcvt, w, s)
 DEF_ASM_WITH_SUFFIXES(fcvt, wu, s) DEF_ASM_WITH_SUFFIXES(fcvt, l, s) DEF_ASM_WITH_SUFFIXES(fcvt, lu, s)
 DEF_ASM_WITH_SUFFIXES(fcvt, s, w) DEF_ASM_WITH_SUFFIXES(fcvt, s, wu) DEF_ASM_WITH_SUFFIXES(fcvt, s, l)
 DEF_ASM_WITH_SUFFIXES(fcvt, s, lu) DEF_ASM_WITH_SUFFIXES(fcvt, w, d) DEF_ASM_WITH_SUFFIXES(fcvt, wu, d)
 DEF_ASM_WITH_SUFFIXES(fcvt, l, d) DEF_ASM_WITH_SUFFIXES(fcvt, lu, d) DEF_ASM_WITH_SUFFIXES(fcvt, d, w)
 DEF_ASM_WITH_SUFFIXES(fcvt, d, wu) DEF_ASM_WITH_SUFFIXES(fcvt, d, l) DEF_ASM_WITH_SUFFIXES(fcvt, d, lu)
 DEF_ASM_WITH_SUFFIXES(fcvt, s, d) DEF_ASM_WITH_SUFFIXES(fcvt, d, s) DEF_ASM_WITH_SUFFIXES(fmv, x, w)
 DEF_ASM_WITH_SUFFIXES(fmv, w, x) DEF_ASM_WITH_SUFFIXES(fmv, x, d) DEF_ASM_WITH_SUFFIXES(fmv, d, x)
 DEF_ASM(cycle) DEF_ASM(fcsr) DEF_ASM(fflags)
 DEF_ASM(frm) DEF_ASM(instret) DEF_ASM(time)
 DEF_ASM(sstatus) DEF_ASM(sie) DEF_ASM(stvec)
 DEF_ASM(scounteren) DEF_ASM(sscratch) DEF_ASM(sepc)
 DEF_ASM(scause) DEF_ASM(stval) DEF_ASM(sip)
 DEF_ASM(satp) DEF_ASM(csrc) DEF_ASM(csrci)
 DEF_ASM(csrr) DEF_ASM(csrs) DEF_ASM(csrsi)
 DEF_ASM(csrw) DEF_ASM(csrwi) DEF_ASM(frcsr)
 DEF_ASM(frflags) DEF_ASM(frrm) DEF_ASM(fscsr)
 DEF_ASM(fsrm) DEF_ASM_WITH_SUFFIX(sfence, vma)
 DEF_ASM(beqz) DEF_ASM(bgez) DEF_ASM(bgt)
 DEF_ASM(bgtu) DEF_ASM(bgtz) DEF_ASM(ble)
 DEF_ASM(bleu) DEF_ASM(blez) DEF_ASM(bltz)
 DEF_ASM(bnez) DEF_ASM(call) DEF_ASM_WITH_SUFFIX(fabs, d)
 DEF_ASM_WITH_SUFFIX(fabs, s) DEF_ASM_WITH_SUFFIX(fmv, d) DEF_ASM_WITH_SUFFIX(fmv, s)
 DEF_ASM_WITH_SUFFIX(fneg, d) DEF_ASM_WITH_SUFFIX(fneg, s) DEF_ASM(j)
 DEF_ASM(jump) DEF_ASM(jr) DEF_ASM(la)
 DEF_ASM(li) DEF_ASM(lla) DEF_ASM(mv)
 DEF_ASM(neg) DEF_ASM(negw) DEF_ASM(nop)
 DEF_ASM(not) DEF_ASM(ret) DEF_ASM(seqz)
 DEF_ASM_WITH_SUFFIX(sext, w) DEF_ASM(sgtz) DEF_ASM(sltz)
 DEF_ASM(snez) DEF_ASM(tail)
#define DEF_ATOMIC_WIDTH(x, w) \
 DEF_ASM_WITH_SUFFIX(x, w) DEF_ASM_WITH_SUFFIXES(x, w, aq) DEF_ASM_WITH_SUFFIXES(x, w, rl) \
 DEF_ASM_WITH_SUFFIXES(x, w, aqrl)
#define DEF_ATOMIC(x) DEF_ATOMIC_WIDTH(x, w) DEF_ATOMIC_WIDTH(x, d)
 DEF_ATOMIC(lr)  DEF_ATOMIC(sc)
 DEF_ATOMIC(amoadd)  DEF_ATOMIC(amoswap)
 DEF_ATOMIC(amoand)  DEF_ATOMIC(amoor)
 DEF_ATOMIC(amoxor)  DEF_ATOMIC(amomax)
 DEF_ATOMIC(amomaxu)  DEF_ATOMIC(amomin)
 DEF_ATOMIC(amominu)
#undef DEF_ATOMIC
#undef DEF_ATOMIC_WIDTH
 DEF_ASM(rne) DEF_ASM(rtz) DEF_ASM(rdn)
 DEF_ASM(rup) DEF_ASM(rmm)
 DEF_ASM_FENCE(w)  DEF_ASM_FENCE(r)
 DEF_ASM_FENCE(rw)  DEF_ASM_FENCE(o)
 DEF_ASM_FENCE(ow)  DEF_ASM_FENCE(or)
 DEF_ASM_FENCE(orw)  DEF_ASM_FENCE(i)
 DEF_ASM_FENCE(iw)  DEF_ASM_FENCE(ir)
 DEF_ASM_FENCE(irw)  DEF_ASM_FENCE(io)
 DEF_ASM_FENCE(iow)  DEF_ASM_FENCE(ior)
 DEF_ASM_FENCE(iorw)
#undef DEF_ASM_FENCE
#undef DEF_ASM_WITH_SUFFIX
#undef DEF_ASM_WITH_SUFFIXES
