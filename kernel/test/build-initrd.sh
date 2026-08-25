#!/bin/sh
set -eu

root=${1:-..}
output=${2:-initrd.tar}
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

cp "$root/kernel/user_test/busybox_riscv64.elf" "$stage/busybox"
for name in user_test hello proc_test proc_fork_test proc_exec_test init_test interactive_test exec_target; do
    cp "$root/kernel/user_test/${name}_riscv64.elf" "$stage/$name"
done
cp "$root/kernel/test/initrd-fixture/greeting" "$stage/greeting"
cp "$root/kernel/test/initrd-fixture/test.sh" "$stage/test.sh"
cp "$root/kernel/test/initrd-fixture/from-initrd.txt" "$stage/from-initrd.txt"
tar cf "$output" -C "$stage" busybox user_test hello proc_test \
    proc_fork_test proc_exec_test init_test interactive_test exec_target \
    greeting test.sh from-initrd.txt
