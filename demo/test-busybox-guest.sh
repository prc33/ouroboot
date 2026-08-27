#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
source_tree=${1:-"$root/busybox-riscv64"}
initrd=$(mktemp)
trap 'rm -f "$initrd"' EXIT

test -f "$source_tree/include/autoconf.h" || {
	echo "Build the configured source tree first: demo/build-busybox-riscv64.sh" >&2
	exit 1
}

make -C "$root/kernel" ARCH=riscv64 selfhost-initrd
BUSYBOX_SRC="$source_tree" sh "$root/kernel/test/build-selfhost-initrd.sh" "$root" "$initrd"
python3 "$root/kernel/test/boot_test.py" "$root/kernel/kernel.elf" \
	--qemu-binary qemu-system-riscv64 --qemu-arg=-M --qemu-arg=virt \
	--qemu-arg=-bios --qemu-arg=default \
	--qemu-arg=-cpu --qemu-arg=rv64,sstc=true \
	--qemu-arg=-device --qemu-arg="loader,file=$initrd,addr=0x84000000" \
	--mem 128 --timeout 300 \
	--stdin-input "\r\rash /build-busybox-guest.sh\rexit 7\r" \
	--must-contain "BUSYBOX: runtime complete" \
	--must-not-contain FATAL --must-not-contain "PAGE FAULT"
