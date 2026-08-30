/* The register machine: allocation, spilling, and materialising a value
   into a register (gv). Compiled for the register targets only.

   wasm has no registers -- it has an operand stack, and one wasm local
   per live value costs nothing to "allocate" -- so it supplies its own
   implementations of this interface in wasm/wasm-value.c and never links
   this file. See registers.h. */
#define USING_GLOBALS
#include "tcc.h"
#include "registers.h"

#define USING_TWO_WORDS(t) (TARGET_SECOND_RETURN_REG(t) != VT_CONST)
#define NODATA_WANTED (nocode_wanted > 0)
#if PTR_SIZE == 4
# define VT_PTRDIFF_T VT_INT
#elif LONG_SIZE == 4
# define VT_PTRDIFF_T VT_LLONG
#else
# define VT_PTRDIFF_T (VT_LONG | VT_LLONG)
#endif

/* Register targets lower control flow as jumps and need no additional
   structural information. Wasm implements these hooks to recover the C
   constructs directly. */
ST_FUNC void gjmp_hint_loop_range(int start, int cont)
{
    (void)start;
    (void)cont;
}

ST_FUNC void gjmp_hint_switch_range(int bodies, int lookup, int end)
{
    (void)bodies;
    (void)lookup;
    (void)end;
}

ST_FUNC void save_regs(int n)
{
    SValue *p, *p1;
    for(p = vstack_base(), p1 = vtop - n; p <= p1; p++)
        save_reg(p->r);
}
/* save r to the memory stack, and mark it as being free */
ST_FUNC void save_reg(int r)
{
    save_reg_upstack(r, 0);
}
ST_FUNC void save_reg_upstack(int r, int n)
{
    int l, size, align, bt;
    SValue *p, *p1, sv;

    if ((r &= VT_VALMASK) >= VT_CONST)
        return;
    if (nocode_wanted)
        return;
    l = 0;
    for(p = vstack_base(), p1 = vtop - n; p <= p1; p++) {
        if ((p->r & VT_VALMASK) == r || p->r2 == r) {
            /* must save value on stack if not already done */
            if (!l) {
                bt = p->type.t & VT_BTYPE;
                if (bt == VT_VOID)
                    continue;
                if ((p->r & VT_LVAL) || bt == VT_FUNC)
                    bt = VT_PTR;
                sv.type.t = bt;
                size = type_size(&sv.type, &align);
                l = vstack_temp_local(size, align, &loc);
                sv.r = VT_LOCAL | VT_LVAL;
                sv.c.i = l;
                store(p->r & VT_VALMASK, &sv);
#if defined(TCC_TARGET_I386)
                /* x86 specific: need to pop fp register ST0 if saved */
                if (r == TREG_ST0) {
                    o(0xd8dd); /* fstp %st(0) */
                }
#endif
                /* special long long case */
                if (p->r2 < VT_CONST && USING_TWO_WORDS(bt)) {
                    sv.c.i += PTR_SIZE;
                    store(p->r2, &sv);
                }
            }
            /* mark that stack entry as being saved on the stack */
            if (p->r & VT_LVAL) {
                p->r = (p->r & ~VT_VALMASK) | VT_LLOCAL;
            } else {
                p->r = VT_LVAL | VT_LOCAL;
            }
            p->r2 = VT_CONST;
            p->c.i = l;
        }
    }
}
/* find a free register of class 'rc'. If none, save one register */
ST_FUNC int get_reg(int rc)
{
    int r;
    SValue *p;

    /* find a free register */
    for(r=0;r<NB_REGS;r++) {
        if (reg_classes[r] & rc) {
            if (nocode_wanted)
                return r;
            for(p=vstack_base();p<=vtop;p++) {
                if ((p->r & VT_VALMASK) == r ||
                    p->r2 == r)
                    goto notfound;
            }
            return r;
        }
    notfound: ;
    }

    /* no register left : free the first one on the stack (VERY
       IMPORTANT to start from the bottom to ensure that we don't
       spill registers used in gen_opi()) */
                for(p=vstack_base();p<=vtop;p++) {
        /* look at second register (if long long) */
        r = p->r2;
        if (r < VT_CONST && (reg_classes[r] & rc))
            goto save_found;
        r = p->r & VT_VALMASK;
        if (r < VT_CONST && (reg_classes[r] & rc)) {
        save_found:
            save_reg(r);
            return r;
        }
    }
    /* Should never comes here */
    return -1;
}
ST_FUNC void move_reg(int r, int s, int t)
{
    SValue sv;

    if (r != s) {
        save_reg(r);
        sv.type.t = t;
        sv.type.ref = NULL;
        sv.r = s;
        sv.c.i = 0;
        load(r, &sv);
    }
}
ST_FUNC int gv(int rc)
{
    int r, r2, r_ok, r2_ok, rc2, bt;
    int bit_pos, bit_size, size, align;

    /* NOTE: get_reg can modify vstack[] */
    if (vtop->type.t & VT_BITFIELD) {
        CType type;

        bit_pos = BIT_POS(vtop->type.t);
        bit_size = BIT_SIZE(vtop->type.t);
        /* remove bit field info to avoid loops */
        vtop->type.t &= ~VT_STRUCT_MASK;

        type.ref = NULL;
        type.t = vtop->type.t & VT_UNSIGNED;
        if ((vtop->type.t & VT_BTYPE) == VT_BOOL)
            type.t |= VT_UNSIGNED;

        r = adjust_bf(vtop, bit_pos, bit_size);

        if ((vtop->type.t & VT_BTYPE) == VT_LLONG)
            type.t |= VT_LLONG;
        else
            type.t |= VT_INT;

        if (r == VT_STRUCT) {
            load_packed_bf(&type, bit_pos, bit_size);
        } else {
            int bits = (type.t & VT_BTYPE) == VT_LLONG ? 64 : 32;
            /* cast to int to propagate signedness in following ops */
            gen_cast(&type);
            /* generate shifts */
            vpushi(bits - (bit_pos + bit_size));
            gen_op(TOK_SHL);
            vpushi(bits - bit_size);
            /* NOTE: transformed to SHR if unsigned */
            gen_op(TOK_SAR);
        }
        r = gv(rc);
    } else {
        if (is_float(vtop->type.t) &&
            (vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST) {
            unsigned long offset;
            /* CPUs usually cannot use float constants, so we store them
               generically in data segment */
            size = type_size(&vtop->type, &align);
            if (NODATA_WANTED)
                size = 0, align = 1;
            offset = section_add(data_section, size, align);
            vpush_ref(&vtop->type, data_section, offset, size);
	    vswap();
	    init_putv(&vtop->type, data_section, offset);
	    vtop->r |= VT_LVAL;
        }

        bt = vtop->type.t & VT_BTYPE;

        rc = TARGET_ADJUST_REG_CLASS(bt, rc);
        rc2 = RC2_TYPE(bt, rc);

        /* need to reload if:
           - constant
           - lvalue (need to dereference pointer)
           - already a register, but not in the right class */
        r = vtop->r & VT_VALMASK;
        r_ok = !(vtop->r & VT_LVAL) && (r < VT_CONST) && (reg_classes[r] & rc);
        r2_ok = !rc2 || ((vtop->r2 < VT_CONST) && (reg_classes[vtop->r2] & rc2));

        if (!r_ok || !r2_ok) {
            if (!r_ok)
                r = get_reg(rc);
            if (rc2) {
                int load_type = (bt == VT_QFLOAT) ? VT_DOUBLE : VT_PTRDIFF_T;
                int original_type = vtop->type.t;

                /* two register type load :
                   expand to two words temporarily */
                if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST) {
                    /* load constant */
                    unsigned long long ll = vtop->c.i;
                    vtop->c.i = ll; /* first word */
                    load(r, vtop);
                    vtop->r = r; /* save register value */
                    vpushi(ll >> 32); /* second word */
                } else if (vtop->r & VT_LVAL) {
                    /* We do not want to modifier the long long pointer here.
                       So we save any other instances down the stack */
                    save_reg_upstack(vtop->r, 1);
                    /* load from memory */
                    vtop->type.t = load_type;
                    load(r, vtop);
                    vdup();
                    vtop[-1].r = r; /* save register value */
                    /* increment pointer to get second word */
                    vtop->type.t = VT_PTRDIFF_T;
                    gaddrof();
                    vpushs(PTR_SIZE);
                    gen_op('+');
                    vtop->r |= VT_LVAL;
                    vtop->type.t = load_type;
                } else {
                    /* move registers */
                    if (!r_ok)
                        load(r, vtop);
                    if (r2_ok && vtop->r2 < VT_CONST)
                        goto done;
                    vdup();
                    vtop[-1].r = r; /* save register value */
                    vtop->r = vtop[-1].r2;
                }
                /* Allocate second register. Here we rely on the fact that
                   get_reg() tries first to free r2 of an SValue. */
                r2 = get_reg(rc2);
                load(r2, vtop);
                vpop();
                /* write second register */
                vtop->r2 = r2;
            done:
                vtop->type.t = original_type;
            } else {
                if (vtop->r == VT_CMP)
                    vset_VT_JMP();
                /* one register type load */
                load(r, vtop);
            }
        }
        vtop->r = r;
    }
    return r;
}
/* generate vtop[-1] and vtop[0] in resp. classes rc1 and rc2 */
ST_FUNC void gv2(int rc1, int rc2)
{
    /* generate more generic register first. But VT_JMP or VT_CMP
       values must be generated first in all cases to avoid possible
       reload errors */
    if (vtop->r != VT_CMP && rc1 <= rc2) {
        vswap();
        gv(rc1);
        vswap();
        gv(rc2);
        /* test if reload is needed for first register */
        if ((vtop[-1].r & VT_VALMASK) >= VT_CONST) {
            vswap();
            gv(rc1);
            vswap();
        }
    } else {
        gv(rc2);
        vswap();
        gv(rc1);
        vswap();
        /* test if reload is needed for first register */
        if ((vtop[0].r & VT_VALMASK) >= VT_CONST) {
            gv(rc2);
        }
    }
}
ST_FUNC void gv_dup(void)
{
    int t, rc, r;

    t = vtop->type.t;
#ifdef TCC_TARGET_I386
    if ((t & VT_BTYPE) == VT_LLONG) {
        i386_gv_dup_llong(t);
        return;
    }
#endif
    /* duplicate value */
    rc = RC_TYPE(t);
    gv(rc);
    r = get_reg(rc);
    vdup();
    load(r, vtop);
    vtop->r = r;
}
