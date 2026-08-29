#ifndef TCC_UTIL_GEN_H
#define TCC_UTIL_GEN_H

#include "target.h"

struct switch_case {
    int64_t v1, v2;
    int sym;
};

struct switch_gen_ops {
    int (*jump)(int);
    void (*jump_addr)(int, int);
    void (*sym)(int);
    int op_le, op_ge, op_eq;
};

void gen_switch(struct switch_case **base, int len, int *bsym, int is_ll,
                const struct switch_gen_ops *ops);

#endif
