#!/bin/sh
set -eu

root=${1:-..}
output=${2:-selfhost-initrd.tar}
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

mkdir -p "$stage/tcc-src/include" "$stage/tcc-src/i386" \
    "$stage/tcc-src/risc" "$stage/tcc-src/wasm" "$stage/musl/obj/include" \
    "$stage/musl/include" "$stage/musl/arch/riscv64" \
    "$stage/musl/arch/generic" "$stage/musl/lib"

# Only busybox itself -- kmain.c's product boot execve()s real argv
# {"ash","-i"} straight against it (see that file's own comment), no
# separate wrapper ELF needed. kernel.elf boots straight to an
# interactive shell (no historical checkpoint chain; see
# docs/kernel-complexity-review.md sections 1 and 3), so none of the
# P4-P10 checkpoint fixtures (proc_test, init_test, etc.) this initrd
# used to also carry are ever loaded, execve'd, or referenced by
# RISCV64_SELFHOST_INPUT/selfhost.sh.
cp "$root/kernel/user_test/busybox_riscv64.elf" "$stage/busybox"
cp "$root/kernel/user_test/fetch_riscv64.elf" "$stage/fetch"

cp "$root"/compiler/*.c "$root"/compiler/*.h "$stage/tcc-src/"
cp "$root"/compiler/*.def "$stage/tcc-src/"
cp "$root"/compiler/i386/*.c "$root"/compiler/i386/*.h \
    "$root"/compiler/i386/*.S "$stage/tcc-src/i386/"
cp "$root"/compiler/risc/*.c "$root"/compiler/risc/*.S "$stage/tcc-src/risc/"
cp "$root"/compiler/wasm/*.c "$root"/compiler/wasm/*.h "$stage/tcc-src/wasm/"
cp "$root"/compiler/include/*.h "$stage/tcc-src/include/"
cp "$root"/compiler/libtcc1.a "$stage/tcc-src/"
cp "$root"/compiler/stage1/tcc "$stage/tcc"
cp -R "$root"/musl-riscv64/obj/include/. "$stage/musl/obj/include/"
cp -R "$root"/musl-riscv64/include/. "$stage/musl/include/"
cp -R "$root"/musl-riscv64/arch/riscv64/. "$stage/musl/arch/riscv64/"
cp -R "$root"/musl-riscv64/arch/generic/. "$stage/musl/arch/generic/"
cp "$root"/musl-riscv64/lib/crt1.o "$root"/musl-riscv64/lib/crti.o \
    "$root"/musl-riscv64/lib/crtn.o "$root"/musl-riscv64/lib/libc.a \
    "$stage/musl/lib/"
cp "$root"/kernel/test/selfhost-hello.c "$stage/hello.c"
cp "$root"/kernel/test/selfhost.sh "$stage/"
cp "$root"/kernel/test/build-musl.sh "$stage/"
cp "$root"/kernel/test/tcc-stage2.args "$stage/"

tar cf "$output" -C "$stage" busybox fetch tcc tcc-src musl hello.c \
    selfhost.sh build-musl.sh tcc-stage2.args
