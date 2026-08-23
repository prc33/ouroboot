#!/bin/bash
# Clones the real upstream musl (not a vendored copy) at the exact
# version this project tested against, tries building it under our
# TCC completely unpatched first -- which fails, demonstrating a real
# TCC gap -- then applies patches/musl-tcc-compat.patch and rebuilds
# successfully. See docs/tcc-spike-findings.md for the full narrative
# behind each fix; this script is the reproducible, minimal proof.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
TCC="${TCC:-$HERE/../compiler/tcc}"
MUSL_VERSION=v1.2.4
MUSL_REPO=https://github.com/kraj/musl.git   # tracks upstream musl-libc.org 1:1

if [ ! -x "$TCC" ]; then
	echo "error: $TCC not found." >&2
	echo "Build the tcc repo first (cd ../compiler && make TARGET=i386), or set \$TCC." >&2
	exit 1
fi

echo "=== cloning real upstream musl $MUSL_VERSION (unmodified) ==="
# Cloned as a sibling of tcc/kernel/demo-tcc-gaps (not nested under
# this script's own directory) so it lands where tcc's own
# `make selfcheck MUSL=../musl` and busybox's build.sh already expect
# it by default -- one consistent layout across all of these repos.
MUSL_DIR="$HERE/../musl-i386"
rm -rf "$MUSL_DIR"
git clone -q --depth 1 --branch "$MUSL_VERSION" "$MUSL_REPO" "$MUSL_DIR"
cd "$MUSL_DIR"
echo "HEAD: $(git rev-parse HEAD)"

configure_and_try() {
	rm -rf obj lib config.mak
	CC="$TCC" ./configure --disable-shared --target=i386-linux-musl >/dev/null 2>&1
	sed -i '/^CFLAGS_C99FSE = /s/$/ -DSYSCALL_NO_TLS=1/' config.mak
	sed -i 's/^AR = .*/AR = ar/; s/^RANLIB = .*/RANLIB = ranlib/' config.mak
	# -DSYSCALL_NO_TLS=1 is a compile flag, not a source patch: it
	# selects musl's own fallback syscall path (plain int $0x80
	# instead of the vDSO-cooperative fast-call trampoline), which we
	# want architecturally regardless of TCC -- our kernel has no vDSO.
	make -j"$(nproc)" 2>&1
}

echo ""
echo "=== attempt 1: unpatched upstream musl, straight from git ==="
echo "    (this is expected to fail -- see below for what breaks)"
if configure_and_try > /tmp/musl-unpatched-build.log 2>&1; then
	echo "    unexpectedly succeeded?! (upstream musl may have changed -- check patches/musl-tcc-compat.patch is still needed)"
else
	echo "    FAILED, as expected. First real error:"
	grep -m1 "error:" /tmp/musl-unpatched-build.log | sed 's/^/    /'
	echo "    (full log: /tmp/musl-unpatched-build.log)"
fi

echo ""
echo "=== applying patches/musl-tcc-compat.patch ==="
git apply "$HERE/patches/musl-tcc-compat.patch"
echo "    applied: 3 file modifications (weak_alias, sigsetjmp.s jecxz,"
echo "    sqrtl.c constant-folding), 2 directory removals"
echo "    (src/complex -- no _Complex support in TCC, out of scope anyway;"
echo "     src/math/i386 + src/fenv/i386 -- x87 asm using constraints TCC"
echo "     doesn't implement; portable C fallbacks take over automatically)"

echo ""
echo "=== attempt 2: patched musl ==="
if configure_and_try > /tmp/musl-patched-build.log 2>&1; then
	echo "    SUCCESS: $(ls -la lib/libc.a | awk '{print $5}') bytes, $(ar t lib/libc.a | wc -l) objects"
else
	echo "    FAILED -- this shouldn't happen. See /tmp/musl-patched-build.log"
	tail -30 /tmp/musl-patched-build.log
	exit 1
fi

echo ""
echo "=== end-to-end smoke test: compile+link+run a real program against this libc ==="
cat > /tmp/musl_smoke.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
int main(void) {
	int *buf = malloc(10 * sizeof(int));
	int sum = 0;
	for (int i = 0; i < 10; i++) { buf[i] = i * i; sum += buf[i]; }
	free(buf);
	printf("sum=%d\n", sum);
	return sum == 285 ? 0 : 1;
}
EOF
TCCDIR="$(dirname "$TCC")"
"$TCC" -B"$TCCDIR" -static -nostdlib -o /tmp/musl_smoke \
	lib/crt1.o lib/crti.o /tmp/musl_smoke.c lib/libc.a "$TCCDIR/libtcc1.a" lib/crtn.o \
	-Iobj/include -Iinclude -Iarch/i386 -Iarch/generic -nostdinc
if command -v qemu-i386-static >/dev/null; then
	OUT="$(qemu-i386-static /tmp/musl_smoke)"
	echo "    output: $OUT"
	[ "$OUT" = "sum=285" ] && echo "    OK" || { echo "    WRONG OUTPUT"; exit 1; }
else
	echo "    (qemu-i386-static not found, skipping run -- binary built OK though)"
fi

echo ""
echo "musl built at: $MUSL_DIR/lib/libc.a"
