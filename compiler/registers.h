#ifndef TCC_REGISTERS_H
#define TCC_REGISTERS_H
#include "vstack.h"
ST_FUNC void save_reg(int r);
ST_FUNC void save_reg_upstack(int r, int n);
ST_FUNC void save_regs(int n);
ST_FUNC int get_reg(int rc);
ST_FUNC void move_reg(int r, int s, int t);
ST_FUNC void gaddrof(void);
ST_FUNC int gv(int rc);
ST_FUNC void gv2(int rc1, int rc2);
ST_FUNC void gv_dup(void);
ST_FUNC int adjust_bf(SValue *, int, int);
/* Shared with regalloc.c's gv(), which is a separate translation unit
   (and one wasm does not compile at all -- see registers.h's own note). */
ST_FUNC void load_packed_bf(CType *type, int bit_pos, int bit_size);
ST_FUNC void store_packed_bf(int, int);
#endif
