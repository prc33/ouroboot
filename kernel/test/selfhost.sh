#!/bin/ash
set -e

echo "SELFHOST: stage1 compiling TCC"
. /tcc-stage2.args
/tcc "$@"

echo "SELFHOST: stage2 runs"
/tcc-stage2 -v

echo "SELFHOST: stage2 compiling hello.c"
/tcc-stage2 -B/tcc-src -I/tcc-src/include -I/musl/obj/include -I/musl/include -I/musl/arch/riscv64 -I/musl/arch/generic -nostdinc -static -nostdlib -o /hello /musl/lib/crt1.o /musl/lib/crti.o /hello.c /musl/lib/libc.a /tcc-src/libtcc1.a /musl/lib/crtn.o

echo "SELFHOST: running hello"
/hello
echo "SELFHOST: complete"
