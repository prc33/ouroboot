#ifndef TCC_REGISTERS_H
#define TCC_REGISTERS_H
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
ST_FUNC void store_packed_bf(int, int);
#endif
