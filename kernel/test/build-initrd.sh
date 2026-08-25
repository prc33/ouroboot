#!/bin/sh
set -eu

root=${1:-..}
output=${2:-initrd.tar}
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

cp "$root/kernel/user_test/busybox_riscv64.elf" "$stage/busybox"
cp "$root/kernel/user_test/exec_target_riscv64.elf" "$stage/exec_target"
cp "$root/kernel/test/initrd-fixture/greeting" "$stage/greeting"
cp "$root/kernel/test/initrd-fixture/test.sh" "$stage/test.sh"
cp "$root/kernel/test/initrd-fixture/from-initrd.txt" "$stage/from-initrd.txt"
tar cf "$output" -C "$stage" busybox exec_target greeting test.sh from-initrd.txt

if [ "$#" -ge 3 ]; then
    cp "$3" "$stage/tcc"
    tar rf "$output" -C "$stage" tcc
fi
