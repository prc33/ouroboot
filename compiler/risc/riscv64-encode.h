#ifndef RISCV64_ENCODE_H
#define RISCV64_ENCODE_H
#define RV_RD(r) ((uint32_t)(r) << 7)
#define RV_RS1(r) ((uint32_t)(r) << 15)
#define RV_RS2(r) ((uint32_t)(r) << 20)
static inline uint32_t rv_r(unsigned op, unsigned f3, unsigned f7,
 unsigned rd, unsigned rs1, unsigned rs2) { return op | RV_RD(rd) |
 (f3 << 12) | RV_RS1(rs1) | RV_RS2(rs2) | (f7 << 25); }
static inline uint32_t rv_i(unsigned op, unsigned f3, unsigned rd,
 unsigned rs1, unsigned imm) { return op | RV_RD(rd) | (f3 << 12) |
 RV_RS1(rs1) | (imm << 20); }
static inline uint32_t rv_s(unsigned op, unsigned f3, unsigned rs1,
 unsigned rs2, unsigned imm) { return op | ((imm & 31) << 7) |
 (f3 << 12) | RV_RS1(rs1) | RV_RS2(rs2) | ((imm >> 5) << 25); }
static inline uint32_t rv_u(unsigned op, unsigned rd, unsigned imm)
{ return op | RV_RD(rd) | (imm & 0xfffff000); }

#define RV_INSN(name, spelling, format, bits) RV_##name = bits,
enum {
#include "riscv64-insns.h"
};
#undef RV_INSN
#endif
