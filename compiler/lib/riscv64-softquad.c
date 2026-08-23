/* Minimal 128-bit long double (quad) helpers for riscv64.
 *
 * The RISC-V lp64d ABI makes `long double` a 128-bit quad, but no
 * common hardware implements quad float, so the compiler is expected to
 * call libgcc/compiler-rt soft-float helpers. TCC ships neither for
 * this target (lib/libtcc1.c has no riscv64 support at all -- it errors
 * "unsupported CPU type"), and musl does not provide them either.
 *
 * These implementations back `long double` with a plain `double`, which
 * is exactly the tradeoff already made for x87 on the i386 target (see
 * plan decision D9). Consequence: `long double` has double precision,
 * not quad. That is visible only in printf("%.20Lf")-style output and
 * in libm's *l() functions; nothing in the kernel, busybox, or TCC
 * itself depends on quad precision.
 *
 * Only the helpers actually referenced by our build are implemented.
 * Anything else will surface as a link error naming the missing symbol,
 * which is the right failure mode -- better than silently wrong math.
 */

typedef long double ld_t;

/* double -> long double */
ld_t __extenddftf2(double a) { return (ld_t)a; }
/* float -> long double */
ld_t __extendsftf2(float a) { return (ld_t)a; }
/* long double -> double */
double __trunctfdf2(ld_t a) { return (double)a; }
/* long double -> float */
float __trunctfsf2(ld_t a) { return (float)a; }

ld_t __addtf3(ld_t a, ld_t b) { return a + b; }
ld_t __subtf3(ld_t a, ld_t b) { return a - b; }
ld_t __multf3(ld_t a, ld_t b) { return a * b; }
ld_t __divtf3(ld_t a, ld_t b) { return a / b; }
ld_t __negtf2(ld_t a)         { return -a; }

/* comparisons: return <0, 0, >0 like the libgcc contract */
int __eqtf2(ld_t a, ld_t b) { return !(a == b); }
int __netf2(ld_t a, ld_t b) { return !(a == b); }
int __lttf2(ld_t a, ld_t b) { return a < b ? -1 : (a == b ? 0 : 1); }
int __letf2(ld_t a, ld_t b) { return a < b ? -1 : (a == b ? 0 : 1); }
int __gttf2(ld_t a, ld_t b) { return a > b ? 1 : (a == b ? 0 : -1); }
int __getf2(ld_t a, ld_t b) { return a > b ? 1 : (a == b ? 0 : -1); }
int __unordtf2(ld_t a, ld_t b) { return (a != a) || (b != b); }

/* integer conversions */
ld_t __floatsitf(int a)            { return (ld_t)a; }
ld_t __floatditf(long a)           { return (ld_t)a; }
ld_t __floatunsitf(unsigned int a) { return (ld_t)a; }
ld_t __floatunditf(unsigned long a){ return (ld_t)a; }
int  __fixtfsi(ld_t a)             { return (int)a; }
long __fixtfdi(ld_t a)             { return (long)a; }
unsigned int  __fixunstfsi(ld_t a) { return (unsigned int)a; }
unsigned long __fixunstfdi(ld_t a) { return (unsigned long)a; }
