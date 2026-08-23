# Week-1 spike: does musl build under TCC? — RESOLVED, GO

**Verdict: yes.** Unmodified upstream musl 1.2.4, targeting i386, builds to a complete
`libc.a` under an i386-targeting TCC with zero compiler errors, using a 4-file patch
(`patches/musl-tcc-compat.patch`, 27 lines). A real program (malloc, snprintf, strcmp)
compiled against that libc.a, linked by TCC's own linker, runs correctly under
`qemu-i386-static` with the correct output and exit code. This clears the single
highest-risk item in the project plan.

## Method

1. Pulled TCC 0.9.27 and musl 1.2.4 source via `apt-get source` (Ubuntu 24.04 universe).
2. Built TCC configured `--cpu=i386` — an i386-*targeting* compiler (the `tcc` binary
   itself runs as x86_64; its codegen backend emits i386 machine code, which is what
   matters per D1).
3. `./configure --target=i386-linux-musl` for musl, `CC=<our tcc>`, `--disable-shared`.
4. `make -j1 -k`, iterating on each distinct error class until the build reached
   `EXIT:0` with a real `libc.a`.
5. End-to-end proof: compiled a test program against that libc.a with the same TCC,
   statically linked, ran under `qemu-i386-static`.

## Findings, in the order encountered

### 1. `_Complex` type unsupported — expected, out of scope
`complex.h` fails to parse (`';' expected (got "cacos")`) because TCC has no
`_Complex` type at all. **Fix:** drop `src/complex` from the build (musl's glob-based
source discovery means this is just `rm -rf`). We don't need libm complex math for
busybox/bash/curl/tcc. No patch required, no functionality lost for our scope.

### 2. x86_64 syscall macros hit a register-allocator limit — irrelevant to us
On x86_64, `__syscall4` through `__syscall6` (using `register long r10 __asm__("r10")`
style pinned-register operands with generic `"r"` constraints) fail with
`asm constraint N ('r') could not be satisfied`. Likewise `src/math/x86_64/*` SSE/x87
intrinsics fail on `'x'`/`'t'`/`'X'` constraints and an unrecognized `stmxcsr` opcode.

**This is confined entirely to the x86_64 backend and macros.** D1 locks us to i386,
so this whole category is moot — confirmed by testing musl's *actual* i386
`syscall_arch.h` next.

### 3. musl's real i386 syscall macros — compile clean, verified by disassembly
`arch/i386/syscall_arch.h`, unmodified, compiles with zero errors under our i386 TCC.
Disassembled `__syscall6` (the hardest case — 6 fixed-register operands, which is
literally all of i386's general-purpose registers except `%eax`/`%esp`) and confirmed
correct codegen: musl's own push/pop-`%ebp` trick for the 6th argument works exactly
as intended. **This was the risk register's top line item. Resolved for the actual
target architecture.**

### 4. `weak_alias` — confirmed broken, root-caused, fixed with one macro
`__attribute__((weak, alias("target")))` compiles without error but the aliased symbol
never appears in the object file. Traced into TCC's source (`tccgen.c`): TCC implements
the `alias` attribute as an **intra-translation-unit rename** (rewriting references to
the target name within the same file), not as GCC's semantics of emitting a second real
ELF symbol at the same address. Verified with a minimal two-file reproduction: `a.c`
defines `real_impl` and aliases it to `public_name`; `b.c` calls `public_name()`; link
fails with `undefined symbol 'public_name'`.

**Fix:** TCC's *assembler* correctly implements the GAS `.weak` and `.set` directives
(confirmed by grepping `tcctok.h`'s directive table). Routing `weak_alias` through
`__asm__(".weak new\n.set new, old\n")` instead of the C attribute produces a real
WEAK-bound ELF symbol. Verified end-to-end: two-file link succeeds, and the linked,
statically-linked binary — run under `qemu-i386-static` — returns the correct value,
proving the alias resolves to the right function at runtime, not just in the symbol
table.

musl has exactly **one** definition of the `weak_alias` macro (`src/include/features.h`)
used at 295 call sites, so this is a one-time, four-line patch guarded by `#ifdef
__TINYC__` — every call site inherits the fix automatically.

### 5. `call *%gs:16` (vDSO fast-syscall trampoline) — moot by design
musl's default (non-`SYSCALL_NO_TLS`) syscall path calls through a function pointer
Linux's vDSO installs at a fixed TLS offset, as a fast-path optimization. TCC's
assembler rejects the `%gs:` segment-prefixed indirect call (`error: incorrect prefix`),
and this one macro is transitively included by nearly everything, so it initially
looked like ~550 broken files. **It collapsed to a non-issue**: our kernel isn't
implementing a vDSO (not in the plan's scope), so we want musl's plain `int $0x80`
path regardless of whether TCC could assemble the vDSO call. musl ships exactly this
fallback as a build flag: `-DSYSCALL_NO_TLS=1`. No patch to musl needed — a compile
flag we'd want anyway.

### 6. x87-optimized `src/math/i386/*` — dropped by design (matches plan D9)
21 files (`sqrt`, `fabs`, `rint`, `llrint`, `fmod`, `remainder`, and float/long-double
variants) hand-optimize with x87 stack-register constraints (`'t'`/`'u'`) and `st`
clobbers that TCC's inline-asm doesn't support. **Fix:** drop `src/math/i386`. Verified
from musl's own `Makefile` that its arch-override mechanism is purely path-based
(`REPLACED_OBJS` strips `/$(ARCH)/` from the object path and excludes any generic
object whose stripped path collides) — so removing the directory automatically falls
back to the portable C implementations in `src/math/*.c`, no further changes needed.
This is consistent with — not a compromise against — plan decision D9 (x87 backed by
host `double`, no real 80-bit fidelity): the hand-tuned asm doesn't map meaningfully to
our emulator's FPU model anyway.

### 7. `src/fenv/i386/fenv.s` (`stmxcsr`) — dropped, and musl's own fallback is
### exactly what we want
Same unsupported-opcode issue as above, in the floating-point-environment control
functions. **Fix:** drop `src/fenv/i386`; the generic `src/fenv/fenv.c` takes over.
Read it — it's musl's documented **"dummy functions for archs lacking fenv
implementation"** (`feclearexcept`, `fetestexcept`, etc. are all no-ops returning 0).
This is the *correct* choice for us, not a lesser one: we're not modeling real x87
exception/rounding-mode hardware state, so real fenv semantics would be dishonest
about what our emulator actually does.

### 8. `jecxz` — a genuine, narrow TCC assembler gap
`src/signal/i386/sigsetjmp.s` uses `jecxz` (jump if `%ecx` is zero), a real, valid i386
instruction TCC's assembler doesn't support (misreports as `invalid displacement`,
confirmed via isolated repro). This is the one true "TCC is missing an instruction"
finding from the whole spike. **Fix:** two-instruction substitute,
`test %ecx,%ecx` / `jz`, semantically identical, no behavior change. Chosen over
patching TCC's assembler to keep TCC's own codebase untouched, per the "neat
simplicity" objective — one instruction pair in one file beats extending an assembler
for a rarely-used mnemonic.

### 9. `static const` initializer referencing another `static const` — narrow gap
`src/math/sqrtl.c` line 230: `static const u128 threel = {.hi = three<<32, ...}` where
`three` is itself an earlier `static const`. GCC constant-folds this; TCC's simpler
front-end requires initializers to be literal constants, not expressions over named
objects (even compile-time-constant ones). **Fix:** inline the literal
(`0xc0000000ULL<<32`) instead of referencing `three`. No behavior change — this is deep
inside the long-double software sqrt algorithm, already out of scope for bit-exact
fidelity per D9.

### 10. Build plumbing (not TCC issues)
`AR`/`RANLIB` in musl's generated `config.mak` default to a `$(CROSS_COMPILE)`-prefixed
name (`i386-linux-musl-ar`) that doesn't exist on a non-cross host. Pointed both at the
host `ar`/`ranlib`. Unrelated to TCC; would hit any from-scratch cross build.

## Disposition against the plan

| Plan risk register row | Status |
|---|---|
| "musl won't build under TCC (weak_alias, asm constraints)" — **blocks everything** | **Resolved.** Both named failure modes reproduced, root-caused, and fixed. Total patch: 4 files, 27 lines. |
| "TCC inline asm vs musl expectations" | **Resolved for i386** (the only target that matters per D1). x86_64-only failures are moot. |

No fatal blocker found. Everything encountered was either out of scope by an existing
plan decision (D1, D9), architecturally moot (no vDSO), or fixed with a small, verified,
mechanical patch. **The spike is GO.**

## Artifacts

- `patches/musl-tcc-compat.patch` — the complete, minimal patch (4 files, 27 lines).
- Build recipe: `configure --target=i386-linux-musl CC=<i386-targeting tcc>
  --disable-shared`, then bake `-DSYSCALL_NO_TLS=1` into `CFLAGS_C99FSE` in
  `config.mak`, fix `AR`/`RANLIB`, `rm -rf src/complex src/math/i386 src/fenv/i386`,
  `make -j1`.
- End-to-end test: `hello.c` (malloc/snprintf/strcmp) → compiled and statically linked
  by our TCC against the resulting `libc.a` and TCC's own `libtcc1.a` → runs correctly
  under `qemu-i386-static`.

## Carried forward into P0/P3 (instruction histogram)

`jecxz` should be added to the "confirm TCC's assembler either supports it or we've
patched around it" checklist, though it's now moot for musl itself. Worth a quick grep
of busybox/bash/curl source for the same mnemonic before assuming it's fully behind us.
