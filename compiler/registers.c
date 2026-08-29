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


/* save r to the memory stack, and mark it as being free,
   if seen up to (vtop - n) stack entry */



/* move register 's' (of type 't') to 'r', and flush previous value of r to memory
   if needed */

/* get address of vtop (vtop MUST BE an lvalue) */
ST_FUNC void gaddrof(void)
{
    vtop->r &= ~VT_LVAL;
    /* tricky: if saved lvalue, then we can go back to lvalue */
    if ((vtop->r & VT_VALMASK) == VT_LLOCAL)
        vtop->r = (vtop->r & ~VT_VALMASK) | VT_LOCAL | VT_LVAL;
}


static void incr_bf_adr(int o)
{
    vtop->type = char_pointer_type;
    gaddrof();
    vpushs(o);
    gen_op('+');
    vtop->type.t = VT_BYTE | VT_UNSIGNED;
    vtop->r |= VT_LVAL;
}

/* single-byte load mode for packed or otherwise unaligned bitfields */
ST_FUNC void load_packed_bf(CType *type, int bit_pos, int bit_size)
{
    int n, o, bits;
    save_reg_upstack(vtop->r, 1);
    vpush64(type->t & VT_BTYPE, 0); // B X
    bits = 0, o = bit_pos >> 3, bit_pos &= 7;
    do {
        vswap(); // X B
        incr_bf_adr(o);
        vdup(); // X B B
        n = 8 - bit_pos;
        if (n > bit_size)
            n = bit_size;
        if (bit_pos)
            vpushi(bit_pos), gen_op(TOK_SHR), bit_pos = 0; // X B Y
        if (n < 8)
            vpushi((1 << n) - 1), gen_op('&');
        gen_cast(type);
        if (bits)
            vpushi(bits), gen_op(TOK_SHL);
        vrotb(3); // B Y X
        gen_op('|'); // B X
        bits += n, bit_size -= n, o = 1;
    } while (bit_size);
    vswap(), vpop();
    if (!(type->t & VT_UNSIGNED)) {
        n = ((type->t & VT_BTYPE) == VT_LLONG ? 64 : 32) - bits;
        vpushi(n), gen_op(TOK_SHL);
        vpushi(n), gen_op(TOK_SAR);
    }
}

/* single-byte store mode for packed or otherwise unaligned bitfields */
ST_FUNC void store_packed_bf(int bit_pos, int bit_size)
{
    int bits, n, o, m, c;

    c = (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    vswap(); // X B
    save_reg_upstack(vtop->r, 1);
    bits = 0, o = bit_pos >> 3, bit_pos &= 7;
    do {
        incr_bf_adr(o); // X B
        vswap(); //B X
        c ? vdup() : gv_dup(); // B V X
        vrott(3); // X B V
        if (bits)
            vpushi(bits), gen_op(TOK_SHR);
        if (bit_pos)
            vpushi(bit_pos), gen_op(TOK_SHL);
        n = 8 - bit_pos;
        if (n > bit_size)
            n = bit_size;
        if (n < 8) {
            m = ((1 << n) - 1) << bit_pos;
            vpushi(m), gen_op('&'); // X B V1
            vpushv(vtop-1); // X B V1 B
            vpushi(m & 0x80 ? ~m & 0x7f : ~m);
            gen_op('&'); // X B V1 B1
            gen_op('|'); // X B V2
        }
        vdup(), vtop[-1] = vtop[-2]; // X B B V2
        vstore(), vpop(); // X B
        bits += n, bit_size -= n, bit_pos = 0, o = 1;
    } while (bit_size);
    vpop(), vpop();
}

ST_FUNC int adjust_bf(SValue *sv, int bit_pos, int bit_size)
{
    int t;
    if (0 == sv->type.ref)
        return 0;
    t = sv->type.ref->auxtype;
    if (t != -1 && t != VT_STRUCT) {
        sv->type.t = (sv->type.t & ~(VT_BTYPE | VT_LONG)) | t;
        sv->r |= VT_LVAL;
    }
    return t;
}

/* store vtop a register belonging to class 'rc'. lvalues are
   converted to values. Cannot be used if cannot be converted to
   register value (such as structures). */



/* convert stack entry to register and duplicate its value in another
   register */
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
