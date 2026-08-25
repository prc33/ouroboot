#!/bin/sh
set -eu

root=${1:-..}
output=${2:-selfhost-initrd.tar}
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

mkdir -p "$stage/tcc-src/include" "$stage/musl/obj/include" \
    "$stage/musl/include" "$stage/musl/arch/riscv64" \
    "$stage/musl/arch/generic" "$stage/musl/lib"

cp "$root/kernel/user_test/busybox_riscv64.elf" "$stage/busybox"
cp "$root/kernel/user_test/exec_target_riscv64.elf" "$stage/exec_target"
cp "$root/kernel/test/initrd-fixture/greeting" "$stage/greeting"
cp "$root/kernel/test/initrd-fixture/test.sh" "$stage/test.sh"
cp "$root/kernel/test/initrd-fixture/from-initrd.txt" "$stage/from-initrd.txt"

cp "$root"/compiler/*.c "$root"/compiler/*.h "$stage/tcc-src/"
cp "$root"/compiler/*.def "$stage/tcc-src/"
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
cp "$root"/kernel/test/selfhost-hello.c "$stage/"

tar cf "$output" -C "$stage" busybox exec_target greeting test.sh \
    from-initrd.txt tcc tcc-src musl selfhost-hello.c
