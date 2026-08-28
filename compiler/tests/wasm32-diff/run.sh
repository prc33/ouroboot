#!/bin/bash
# Differential test: compiles compiler/tests/wasm32-diff/corpus.c with
# host gcc (the reference -- an independent implementation, not TCC's
# own front end, so it can't share a bug with either backend) and with
# wasm32 tcc, runs both, and diffs the results function-by-function.
#
# Why this exists at all: every wasm-affecting codegen change so far
# has been verified by rv64.wasm byte-identity against a known-good
# baseline, which only proves "didn't change" -- exactly the wrong
# property to check for a change that's SUPPOSED to emit different
# (better, or just different-shaped) code. This is the harness that
# was missing; see docs/wasm-backend-size-2026-08-28.md's own
# "prerequisite" section.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
COMPILER_DIR="$HERE/../.."
TCC="${TCC:-$COMPILER_DIR/wasm32-tcc}"

if [ ! -x "$TCC" ]; then
	echo "building wasm32 tcc..."
	make -C "$COMPILER_DIR" TARGET=wasm32 >/dev/null
fi

cd "$HERE"

echo "=== generating native reference driver from corpus.c's own test_* functions ==="
python3 gen_driver.py corpus.c > driver.c
grep -c '^int test_' corpus.c | xargs -I{} echo "  {} test functions found"

echo "=== building native reference (host gcc, -O0 -- match TCC's own lack of optimisation) ==="
gcc -O0 -o native_ref corpus.c driver.c
./native_ref | sort > native.out

echo "=== building wasm32 (tcc) ==="
"$TCC" -nostdlib -o corpus.wasm corpus.c

echo "=== running under node ==="
node run_wasm.mjs | sort > wasm.out

echo "=== diffing ==="
if diff -u native.out wasm.out; then
	n=$(wc -l < native.out)
	echo "PASS: $n/$n functions match between host gcc and wasm32 tcc"
else
	echo "FAIL: wasm32 tcc diverges from host gcc -- see diff above"
	exit 1
fi
