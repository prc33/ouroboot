/* Minimal 128-bit long double (quad) helpers for riscv64.
 *
 * The RISC-V lp64d ABI makes `long double` a 128-bit quad, but no
 * common hardware implements quad float, so the compiler is expected to
 * call libgcc/compiler-rt soft-float helpers. TCC and musl do not provide
 * those helpers for this target.
 *
 * These implementations back `long double` with a plain `double` (see plan
 * decision D9). Consequently, `long double` has double precision,
 * not quad. That is visible only in printf("%.20Lf")-style output and
 * in libm's *l() functions; nothing in the kernel, busybox, or TCC
 * itself depends on quad precision.
 *
 * Only the helpers actually referenced by our build are implemented.
 * Anything else will surface as a link error naming the missing symbol,
 * which is the right failure mode -- better than silently wrong math.
 */

typedef long double ld_t;
typedef unsigned long long u64;

/* Keep the lp64d 128-bit ABI while doing the promised double-precision
 * arithmetic.  Writing `(ld_t)d` here would ask the compiler to call
 * __extenddftf2 again, recursively; construct and inspect the IEEE-754 quad
 * representation explicitly instead. */
static ld_t from_double(double d)
{
    union { double d; u64 u; } in;
    union { ld_t q; u64 w[2]; } out;
    u64 sign, frac;
    unsigned int exp;

    in.d = d;
    sign = in.u >> 63;
    exp = (unsigned int)(in.u >> 52) & 0x7ff;
    frac = in.u & 0xfffffffffffffULL;
    out.w[0] = frac << 60;
    out.w[1] = sign << 63 | frac >> 4;
    if (exp)
        out.w[1] |= (u64)(exp == 0x7ff ? 0x7fff : exp + 15360) << 48;
    return out.q;
}

static double to_double(ld_t q)
{
    union { ld_t q; u64 w[2]; } in;
    union { double d; u64 u; } out;
    u64 sign, frac;
    unsigned int exp, dexp;

    in.q = q;
    sign = in.w[1] >> 63;
    exp = (unsigned int)(in.w[1] >> 48) & 0x7fff;
    frac = (in.w[1] & 0xffffffffffffULL) << 4 | in.w[0] >> 60;
    if (!exp)
        dexp = 0;
    else if (exp == 0x7fff)
        dexp = 0x7ff;
    else if (exp <= 15360)
        dexp = 0;
    else if (exp >= 15360 + 0x7ff)
        dexp = 0x7ff;
    else
        dexp = exp - 15360;
    out.u = sign << 63 | (u64)dexp << 52 | frac;
    return out.d;
}

/* double -> long double */
ld_t __extenddftf2(double a) { return from_double(a); }
/* float -> long double */
ld_t __extendsftf2(float a) { return from_double(a); }
/* long double -> double */
double __trunctfdf2(ld_t a) { return to_double(a); }
/* long double -> float */
float __trunctfsf2(ld_t a) { return (float)to_double(a); }

ld_t __addtf3(ld_t a, ld_t b) { return from_double(to_double(a) + to_double(b)); }
ld_t __subtf3(ld_t a, ld_t b) { return from_double(to_double(a) - to_double(b)); }
ld_t __multf3(ld_t a, ld_t b) { return from_double(to_double(a) * to_double(b)); }
ld_t __divtf3(ld_t a, ld_t b) { return from_double(to_double(a) / to_double(b)); }
ld_t __negtf2(ld_t a)
{
    union { ld_t q; u64 w[2]; } v;
    v.q = a; v.w[1] ^= 1ULL << 63; return v.q;
}

/* comparisons: return <0, 0, >0 like the libgcc contract */
int __eqtf2(ld_t a, ld_t b) { return !(to_double(a) == to_double(b)); }
int __netf2(ld_t a, ld_t b) { return !(to_double(a) == to_double(b)); }
int __lttf2(ld_t a, ld_t b) { double x=to_double(a),y=to_double(b); return x<y?-1:(x==y?0:1); }
int __letf2(ld_t a, ld_t b) { return __lttf2(a, b); }
int __gttf2(ld_t a, ld_t b) { double x=to_double(a),y=to_double(b); return x>y?1:(x==y?0:-1); }
int __getf2(ld_t a, ld_t b) { return __gttf2(a, b); }
int __unordtf2(ld_t a, ld_t b) { double x=to_double(a),y=to_double(b); return (x!=x)||(y!=y); }

/* integer conversions */
ld_t __floatsitf(int a)            { return from_double((double)a); }
ld_t __floatditf(long a)           { return from_double((double)a); }
ld_t __floatunsitf(unsigned int a) { return from_double((double)a); }
ld_t __floatunditf(unsigned long a){ return from_double((double)a); }
int  __fixtfsi(ld_t a)             { return (int)to_double(a); }
long __fixtfdi(ld_t a)             { return (long)to_double(a); }
unsigned int  __fixunstfsi(ld_t a) { return (unsigned int)to_double(a); }
unsigned long __fixunstfdi(ld_t a) { return (unsigned long)to_double(a); }
