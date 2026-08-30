/* wasm's replacement for the register machine (regalloc.c), which this
   target never compiles -- see registers.h and the Makefile.
   The front end calls gv() to mean "materialise this value somewhere I
   can refer to". On a register machine that means picking one of a
   handful of real registers and spilling whatever was in it. wasm has no
   registers at all: it has an operand stack, and as many locals as a
   function cares to declare. Locals are free to add and calls do not
   clobber them, so nothing here ever has to spill, evict, or colour
   anything -- the whole reason regalloc.c is 289 lines.
   What replaces it: a "register" is just an index into a per-function
   pool of wasm locals, handed out to whichever value stack slot needs
   one. get_reg() picks the lowest index no live vstack entry is using,
   which cannot fail while the pool is larger than the deepest the value
   stack ever gets in one expression, and there is no fallback path
   because there is nothing to fall back to. save_reg()/save_regs()
   exist only to satisfy the shared interface and do nothing: a wasm
   local's value survives any call, and a value that is live is never
   handed out again.
   Values still reach the actual wasm operand stack, and mostly stay
   there -- but that happens later, in tccwasm.c's WasmVStack when the
   buffered IR is turned into bytecode. This file only decides which
   local a value would spill to if it needs one. */
#define USING_GLOBALS
#include "../tcc.h"
#include "../registers.h"
#define NODATA_WANTED (nocode_wanted > 0)
/* Vacate local r: any live value sitting in it is written out to a frame
   slot and the value stack entry rewritten to refer to that slot, so the
   value is no longer in a local at all.
   Spilling to memory rather than to another local is not laziness -- it
   is the contract callers rely on. expr.c's ternary calls move_reg() to
   force both arms into one location and then assigns vtop->r itself;
   if save_reg() had merely relocated the old occupant to a different
   local, that assignment would silently point at the wrong one. (It
   does: `(a<b?c+d:c-d) + (c<d?...)` quietly returned 2 instead of 8.)
   Locals being free still buys something -- what is gone next to the
   register machine's version is the two-word register pair, the x87
   stack pop, and the second-register bookkeeping, none of which wasm
   has. */
ST_FUNC void save_reg_upstack(int r, int n)
{
    int l, size, align, bt;
    SValue *p, *p1, sv;
    if ((r &= VT_VALMASK) >= VT_CONST)
        return;
    if (nocode_wanted)
        return;
    l = 0;
    for (p = vstack_base(), p1 = vtop - n; p <= p1; ++p) {
        if ((p->r & VT_VALMASK) != r)
            continue;
        /* One slot serves every entry sharing the local: they all held
           the same value. */
        if (!l) {
            bt = p->type.t & VT_BTYPE;
            if (bt == VT_VOID)
                continue;
            if ((p->r & VT_LVAL) || bt == VT_FUNC)
                bt = VT_PTR;
            sv.type.t = bt;
            sv.type.ref = NULL;
            size = type_size(&sv.type, &align);
            l = vstack_temp_local(size, align, &loc);
            sv.r = VT_LOCAL | VT_LVAL;
            sv.c.i = l;
            sv.sym = NULL;
            store(p->r & VT_VALMASK, &sv);
        }
        /* the entry now names the frame slot, not the local */
        if (p->r & VT_LVAL)
            p->r = (p->r & ~VT_VALMASK) | VT_LLOCAL;
        else
            p->r = VT_LVAL | VT_LOCAL;
        p->r2 = VT_CONST;
        p->c.i = l;
    }
}
ST_FUNC void save_reg(int r) { save_reg_upstack(r, 0); }
ST_FUNC void save_regs(int n)
{
    SValue *p, *p1;
    for (p = vstack_base(), p1 = vtop - n; p <= p1; ++p)
        save_reg(p->r);
}
/* Lowest local in class rc that no live value stack entry is holding.
   The scan is the whole allocator: with no eviction there is no cost
   model to weigh and no spill slot to choose. */
ST_FUNC int get_reg(int rc)
{
    int r;
    SValue *p, *base = vstack_base();
    for (r = 0; r < NB_REGS; ++r) {
        if (!(reg_classes[r] & rc))
            continue;
        for (p = base; p <= vtop; ++p)
            if ((p->r & VT_VALMASK) == r || (p->r2 & VT_VALMASK) == r)
                goto notfree;
        return r;
    notfree: ;
    }
    /* Only reachable if one expression holds more simultaneously live
       values than the pool has locals. Raising NB_REGS is the fix; there
       is deliberately no spill path to hide it. */
    tcc_error("wasm32 backend: out of value locals (raise NB_REGS)");
    return 0;
}
ST_FUNC void move_reg(int r, int s, int t)
{
    SValue sv;
    if (r == s)
        return;
    /* Cannot recurse: r is either already free, or comes straight from
       get_reg() which only ever returns a local nothing live is using. */
    save_reg(r);
    sv.type.t = t;
    sv.type.ref = NULL;
    sv.r = s;
    sv.c.i = 0;
    sv.sym = NULL;
    load(r, &sv);
}
/* Materialise vtop. Compare with regalloc.c's gv(): gone are the
   two-word register pairs (wasm has a native i64, so USING_TWO_WORDS is
   never true here), the spill-and-reload dance, and the second register
   class. What is left is the part that was never about registers --
   bitfield extraction and getting a float constant into memory -- plus
   one load. */
ST_FUNC int gv(int rc)
{
    int r;
    int bit_pos, bit_size, size, align;
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
            tcc_error("wasm32 backend: packed bitfield load is not supported");
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
        return gv(rc);
    }
    if (is_float(vtop->type.t)
        && (vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST) {
        /* A float constant has no immediate form, here or on any other
           target: park it in the data segment and load it back. */
        unsigned long offset;
        size = type_size(&vtop->type, &align);
        if (NODATA_WANTED)
            size = 0, align = 1;
        offset = section_add(data_section, size, align);
        vpush_ref(&vtop->type, data_section, offset, size);
        vswap();
        init_putv(&vtop->type, data_section, offset);
        vtop->r |= VT_LVAL;
    }
    rc = TARGET_ADJUST_REG_CLASS(vtop->type.t & VT_BTYPE, rc);
    /* Reload unless it is already sitting in a local of the right class
       and is not an lvalue (which would still need dereferencing). */
    r = vtop->r & VT_VALMASK;
    if ((vtop->r & VT_LVAL) || r >= VT_CONST || !(reg_classes[r] & rc)) {
        r = get_reg(rc);
        if (vtop->r == VT_CMP)
            vset_VT_JMP();
        load(r, vtop);
    }
    /* Plain assignment even when no load happened, exactly as the register
       machine does it: the value lives in a local now, so every flag
       describing where it used to be -- VT_LVAL, VT_SYM, VT_MUSTCAST --
       is stale and must not survive. */
    vtop->r = r;
    return r;
}
/* Both operands into locals. regalloc.c has to order these carefully so
   that materialising the second cannot evict the first; nothing can be
   evicted here, so the only ordering that matters is the front end's own
   rule that a VT_CMP must be consumed before anything else emits code. */
ST_FUNC void gv2(int rc1, int rc2)
{
    if (vtop->r != VT_CMP) {
        vswap();
        gv(rc1);
        vswap();
        gv(rc2);
    } else {
        gv(rc2);
        vswap();
        gv(rc1);
        vswap();
    }
}
/* Materialise vtop and leave a second copy of it in another local. */
ST_FUNC void gv_dup(void)
{
    int t = vtop->type.t;
    int rc = RC_TYPE(t);
    int r;
    gv(rc);
    r = get_reg(rc);
    vdup();
    load(r, vtop);
    vtop->r = r;
}
