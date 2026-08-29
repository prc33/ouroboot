#include "util-gen.h"

static void gen_switch_part(struct switch_case **base, int len, int *bsym,
                            int is_ll, const struct switch_gen_ops *ops)
{
    struct switch_case *p;
    int e;
    while (len > 8) {
        p = base[len / 2];
        vdup();
        if (is_ll) vpushll(p->v2); else vpushi((int)p->v2);
        gen_op(ops->op_le);
        e = gvtst(1, 0);
        vdup();
        if (is_ll) vpushll(p->v1); else vpushi((int)p->v1);
        gen_op(ops->op_ge);
        ops->jump_addr(gvtst(0, 0), p->sym);
        gen_switch_part(base, len / 2, bsym, is_ll, ops);
        ops->sym(e);
        e = len / 2 + 1;
        base += e; len -= e;
    }
    while (len--) {
        p = *base++;
        vdup();
        if (is_ll) vpushll(p->v2); else vpushi((int)p->v2);
        if (p->v1 == p->v2) {
            gen_op(ops->op_eq);
            ops->jump_addr(gvtst(0, 0), p->sym);
        } else {
            gen_op(ops->op_le);
            e = gvtst(1, 0);
            vdup();
            if (is_ll) vpushll(p->v1); else vpushi((int)p->v1);
            gen_op(ops->op_ge);
            ops->jump_addr(gvtst(0, 0), p->sym);
            ops->sym(e);
        }
    }
    *bsym = ops->jump(*bsym);
}

void gen_switch(struct switch_case **base, int len, int *bsym, int is_ll,
                const struct switch_gen_ops *ops)
{
    gen_switch_part(base, len, bsym, is_ll, ops);
}
