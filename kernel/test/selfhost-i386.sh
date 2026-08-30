#!/bin/ash
# i386 counterpart of selfhost.sh -- same shape (stage1 compiles TCC
# from source, stage2 [the result] compiles hello.c, hello runs), see
# that file's own comment. Kept as a separate file rather than
# parameterizing selfhost.sh itself: this is the one place an i386
# arch string (/musl/arch/i386) is genuinely needed, and duplicating
# a 12-line script is cheaper than adding an argument every call site
# would need to thread through.
set -e

echo "SELFHOST: stage1 compiling TCC"
. /tcc-stage2.args
/tcc "$@"

echo "SELFHOST: stage2 runs"
/tcc-stage2 -v

echo "SELFHOST: stage2 compiling hello.c"
/tcc-stage2 -B/tcc-src -I/tcc-src/include -I/musl/obj/include -I/musl/include -I/musl/arch/i386 -I/musl/arch/generic -nostdinc -static -nostdlib -o /hello /musl/lib/crt1.o /musl/lib/crti.o /hello.c /musl/lib/libc.a /tcc-src/libtcc1.a /musl/lib/crtn.o

echo "SELFHOST: running hello"
/hello
echo "SELFHOST: complete"
