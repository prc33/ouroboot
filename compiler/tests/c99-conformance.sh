#!/bin/sh
# Run the freestanding, portable C99 cases from c-testsuite.
# Usage: c99-conformance.sh /path/to/c-testsuite [compiler]
set -eu
suite=$1
cc=${2:-./tcc}
tmp=${TMPDIR:-/tmp}/ouroboot-c99.$$
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp"
total=0
passed=0
skipped=0
for test in "$suite"/tests/single-exec/*.c; do
    tags=$test.tags
    grep -Eq '^(c89|c99)$' "$tags" || continue
    total=$((total + 1))
    if grep -qx needs-libc "$tags"; then
        skipped=$((skipped + 1))
        continue
    fi
    if "$cc" -std=c99 -nostdinc -c "$test" -o "$tmp/$(basename "$test" .c).o"; then
        passed=$((passed + 1))
    else
        echo "FAIL $test" >&2
    fi
done
echo "C99 portable freestanding cases: $passed/$total passed; $skipped libc cases skipped"
test "$passed" -eq $((total - skipped))
