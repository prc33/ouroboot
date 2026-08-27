#!/bin/ash
set -eu

src=/musl-src
cc=/tcc
lib=/musl-libc.a
cflags='-B/tcc-src -nostdlib -std=c99 -nostdinc -ffreestanding -D_XOPEN_SOURCE=700 -Os -fno-stack-protector'
includes="-I$src/arch/riscv64 -I$src/arch/generic -I$src/obj/src/internal -I$src/src/include -I$src/src/internal -I$src/obj/include -I$src/include"
start=${1:-}

echo 'MUSL: generating headers'
sed -f "$src/tools/mkalltypes.sed" "$src/arch/riscv64/bits/alltypes.h.in" \
    "$src/include/alltypes.h.in" > "$src/obj/include/bits/alltypes.h"
cp "$src/arch/riscv64/bits/syscall.h.in" "$src/obj/include/bits/syscall.h"
sed -n -e s/__NR_/SYS_/p < "$src/arch/riscv64/bits/syscall.h.in" >> "$src/obj/include/bits/syscall.h"
echo '#define VERSION "1.2.4-ouroboot"' > "$src/obj/src/internal/version.h"
test -f "$src/include/aio.h" || { echo 'MUSL: extracted headers missing'; exit 1; }
# Reserve a large contiguous backing run before thousands of object files
# fragment ramfs. Truncation keeps the capacity, so ar can fill it later.
cp /musl.tar "$lib"
: > "$lib"
cp /musl.tar "$lib.tmp"
: > "$lib.tmp"
rm -f /musl-objects.args

for dir in "$src"/src/* "$src"/src/malloc/mallocng; do
    case "$start" in
        '') ;;
        "${dir##*/}") start= ;;
        *) continue ;;
    esac
    set -- $cflags $includes -c
    found=
    replacements=' '
    for file in "$dir"/riscv64/*.c "$dir"/riscv64/*.s "$dir"/riscv64/*.S; do
        case "$file" in *\**) continue;; esac
        replacements="$replacements${file##*/} "
    done
    for file in "$dir"/*.c "$dir"/*.s "$dir"/*.S; do
        case "$file" in *\**) continue;; esac
        base=${file##*/}
        case "$replacements" in *" $base "*) continue;; esac
        set -- "$@" "$file"
        found=1
    done
    for file in "$dir"/riscv64/*.c "$dir"/riscv64/*.s "$dir"/riscv64/*.S; do
        case "$file" in *\**) continue;; esac
        set -- "$@" "$file"
        found=1
    done
    case "$found" in '') continue;; esac
    echo "MUSL: ${dir##*/}"
    out="/musl-obj/${dir##*/}"
    mkdir -p "$out"
    : > "$out/.keep"
    cd "$out"
    "$cc" "$@"
    echo "$out"/*.o >> /musl-objects.args
done

"$cc" -ar rcs "$lib" @/musl-objects.args

echo 'MUSL: building startup objects'
for name in crt1 crti crtn; do
    cd /
    "$cc" $cflags $includes -DCRT -c -o "/musl-$name.o" "$src/crt/$name.c"
done

echo 'MUSL: complete'
ls -l "$lib" /musl-crt1.o /musl-crti.o /musl-crtn.o

echo 'MUSL: linking smoke test'
"$cc" -B/tcc-src -static -nostdinc -nostdlib $includes -o /musl-hello \
    /musl-crt1.o /musl-crti.o /hello.c "$lib" /tcc-src/libtcc1.a /musl-crtn.o
/musl-hello
echo 'MUSL: runtime complete'
