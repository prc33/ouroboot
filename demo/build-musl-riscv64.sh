#!/bin/bash
# Clones real upstream musl at the version this project tested against,
# applies patches/musl-riscv64-tcc-compat.patch, builds it with our
# riscv64-targeting TCC. Unlike the i386 version, this does NOT include
# an "unpatched attempt first" demonstration step -- the riscv64 port
# needed far more than a source patch (a from-scratch intrinsic-based
# assembler replacement, two codegen bugs, a register-mapping bug, and
# two musl-generator bugs), and reproducing that whole story as a
# scripted before/after isn't practical the way the i386 gap was. See
# docs/riscv-port-findings.md for the full narrative instead.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
TCC="${TCC:-$HERE/../compiler/tcc}"
MUSL_VERSION=v1.2.4
MUSL_REPO=https://github.com/kraj/musl.git

if [ ! -x "$TCC" ]; then
	echo "error: $TCC not found." >&2
	echo "Build it first: cd ../compiler && make TARGET=riscv64" >&2
	exit 1
fi

echo "=== cloning real upstream musl $MUSL_VERSION (unmodified) ==="
MUSL_DIR="$HERE/../musl-riscv64"
rm -rf "$MUSL_DIR"
git clone -q --depth 1 --branch "$MUSL_VERSION" "$MUSL_REPO" "$MUSL_DIR"
cd "$MUSL_DIR"
echo "HEAD: $(git rev-parse HEAD)"

echo ""
echo "=== removing src/complex ==="
echo "    Not a source patch -- this is a whole-directory removal (every"
echo "    file in it), so a plain rm is clearer than a diff full of"
echo "    'delete this whole file' hunks. No _Complex support in TCC,"
echo "    out of scope anyway; nothing else in a normal musl build"
echo "    includes complex.h once these .c files are gone."
rm -rf src/complex

echo ""
echo "=== removing whole riscv64-only leaf directories ==="
echo "    Same reasoning as src/complex above -- musl-riscv64-tcc-compat.patch"
echo "    used to carry these as 22 individual 'delete this whole file'"
echo "    diff hunks (a majority of the patch's own line count); a plain"
echo "    rm is clearer for a deletion, whole-directory or not. Each one"
echo "    hand-optimizes a real F/D-extension RISC-V instruction (fsgnj.d,"
echo "    csrc fflags, ...) TCC's own riscv64 backend has no assembler for"
echo "    (compiler/Makefile's own header comment: directives + intrinsics"
echo "    only, no real instruction parser) -- removing the override lets"
echo "    musl's own build fall back to its portable C implementation,"
echo "    same mechanism as e.g. src/math/riscv64/copysign.c's own"
echo "    '#else #include \"../copysign.c\"' branch, just triggered by the"
echo "    override being absent instead of a preprocessor check."
rm -rf src/fenv/riscv64 src/math/riscv64
echo "    src/ldso/riscv64/dlsym.s is the same story but for the dynamic"
echo "    linker specifically -- irrelevant anyway, since this project"
echo "    only ever builds musl -static."
rm -rf src/ldso/riscv64

echo ""
echo "=== removing individual now-obsolete/replaced files ==="
echo "    crt/Scrt1.c/rcrt1.c: PIE/dynamic-executable crt startup variants,"
echo "    unused by a -static-only build (same reasoning as dlsym.s above)."
echo "    src/setjmp/riscv64/{longjmp,setjmp}.S: hand-written GNU-syntax"
echo "    assembly using real instructions TCC can't parse -- replaced by"
echo "    this same patch's own lowercase .s versions, pre-assembled into"
echo "    raw '.long 0x...' words by tools/gen_asm_words.sh (also added by"
echo "    this patch) so TCC's directives-only assembler can still emit them."
echo "    src/signal/riscv64/sigsetjmp.s, src/thread/riscv64/{__unmapself,clone}.s:"
echo "    same real-instruction-TCC-can't-parse reason, but with no"
echo "    replacement at all -- nothing in this project's single-threaded,"
echo "    no-real-pthread_create build ever calls sigsetjmp()/__clone()"
echo "    (removing __unmapself.s specifically is what makes musl fall back"
echo "    to this same patch's now-TCC-safe generic src/thread/__unmapself.c)."
echo "    If that's ever wrong, the linker reports an honest undefined-symbol"
echo "    error, not a silent miscompile."
rm -f crt/Scrt1.c crt/rcrt1.c \
	src/setjmp/riscv64/longjmp.S src/setjmp/riscv64/setjmp.S \
	src/signal/riscv64/sigsetjmp.s \
	src/thread/riscv64/__unmapself.s src/thread/riscv64/clone.s

echo ""
echo "=== applying patches/musl-riscv64-tcc-compat.patch ==="
git apply "$HERE/patches/musl-riscv64-tcc-compat.patch"

echo ""
echo "=== building ==="
CC="$TCC" ./configure --disable-shared --target=riscv64-linux-musl >/dev/null 2>&1
sed -i 's/^AR = .*/AR = ar/; s/^RANLIB = .*/RANLIB = ranlib/' config.mak
make -j"$(nproc)" 2>&1
echo "SUCCESS: $(ls -la lib/libc.a | awk '{print $5}') bytes, $(ar t lib/libc.a | wc -l) objects"

echo ""
echo "=== end-to-end smoke test: malloc, snprintf, and a real setjmp/longjmp round trip ==="
cat > /tmp/musl_rv64_smoke.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
static jmp_buf jb;
static void deep(void){ longjmp(jb, 7); }
int main(void) {
	int *buf = malloc(10 * sizeof(int));
	int sum = 0;
	for (int i = 0; i < 10; i++) { buf[i] = i * i; sum += buf[i]; }
	free(buf);
	char msg[64];
	snprintf(msg, sizeof(msg), "sum=%d\n", sum);
	fputs(msg, stdout);
	int v = setjmp(jb);
	if (!v) deep();
	printf("setjmp/longjmp v=%d\n", v);
	return (sum == 285 && v == 7) ? 0 : 1;
}
EOF
TCCDIR="$(dirname "$TCC")"
"$TCC" -B"$TCCDIR" -static -nostdlib -o /tmp/musl_rv64_smoke \
	lib/crt1.o lib/crti.o /tmp/musl_rv64_smoke.c lib/libc.a "$TCCDIR/libtcc1.a" lib/crtn.o \
	-Iobj/include -Iinclude -Iarch/riscv64 -Iarch/generic -nostdinc
if command -v qemu-riscv64-static >/dev/null; then
	qemu-riscv64-static /tmp/musl_rv64_smoke
	[ $? -eq 0 ] && echo "OK" || { echo "FAILED"; exit 1; }
else
	echo "(qemu-riscv64-static not found, skipping run -- binary built OK though)"
fi

echo ""
echo "musl built at: $MUSL_DIR/lib/libc.a"
