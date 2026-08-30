#ifndef TCC_REGISTERS_H
#define TCC_REGISTERS_H
#include "vstack.h"
ST_FUNC void save_reg(int r);
ST_FUNC void save_reg_upstack(int r, int n);
ST_FUNC void save_regs(int n);
ST_FUNC int get_reg(int rc);
ST_FUNC void move_reg(int r, int s, int t);
ST_FUNC int gv(int rc);
ST_FUNC void gv2(int rc1, int rc2);
ST_FUNC void gv_dup(void);
#endif
