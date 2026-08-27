#!/bin/sh
# i386 counterpart of build-selfhost-initrd.sh -- same shape (real
# BusyBox, real TCC sources, real musl, a stage1 TCC binary already
# compiled for the target the kernel will actually run it under), see
# that file's own comment for why each piece is there.
set -eu

root=${1:-..}
output=${2:-selfhost-initrd-i386.tar}
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

mkdir -p "$stage/tcc-src/include" "$stage/tcc-src/i386" \
    "$stage/tcc-src/risc" "$stage/tcc-src/wasm" "$stage/musl/obj/include" \
    "$stage/musl/include" "$stage/musl/arch/i386" \
    "$stage/musl/arch/generic" "$stage/musl/lib"

cp "$root/kernel/user_test/busybox_i386.elf" "$stage/busybox"

cp "$root"/compiler/*.c "$root"/compiler/*.h "$stage/tcc-src/"
cp "$root"/compiler/*.def "$stage/tcc-src/"
cp "$root"/compiler/i386/*.c "$root"/compiler/i386/*.h \
    "$root"/compiler/i386/*.S "$stage/tcc-src/i386/"
# riscv64 has no integrated assembler for real instructions (see
# compiler/Makefile's own header comment) and, since
# risc/fetch_and_add_riscv64.S was deleted as dead weight (unused by
# any target's build -- docs/repo-review-2026-08-26.md section 3),
# no .S file at all: an unmatched *.S glob here would fail outright.
cp "$root"/compiler/risc/*.c "$stage/tcc-src/risc/"
cp "$root"/compiler/wasm/*.c "$root"/compiler/wasm/*.h "$stage/tcc-src/wasm/"
cp "$root"/compiler/include/*.h "$stage/tcc-src/include/"
cp "$root"/compiler/libtcc1.a "$stage/tcc-src/"
cp "$root"/compiler/stage1/tcc "$stage/tcc"
cp -R "$root"/musl-i386/obj/include/. "$stage/musl/obj/include/"
cp -R "$root"/musl-i386/include/. "$stage/musl/include/"
cp -R "$root"/musl-i386/arch/i386/. "$stage/musl/arch/i386/"
cp -R "$root"/musl-i386/arch/generic/. "$stage/musl/arch/generic/"
cp "$root"/musl-i386/lib/crt1.o "$root"/musl-i386/lib/crti.o \
    "$root"/musl-i386/lib/crtn.o "$root"/musl-i386/lib/libc.a \
    "$stage/musl/lib/"
cp "$root"/kernel/test/selfhost-hello.c "$stage/hello.c"
cp "$root"/kernel/test/selfhost-i386.sh "$stage/selfhost.sh"
cp "$root"/kernel/test/tcc-stage2-i386.args "$stage/tcc-stage2.args"

tar cf "$output" -C "$stage" busybox tcc tcc-src musl hello.c selfhost.sh tcc-stage2.args
