#!/bin/ash
set -eu

src=${BUSYBOX_SRC:-/busybox-src}
cc=${CC:-/tcc}
musl=${MUSL:-/musl}
tcc_src=${TCC_SRC:-/tcc-src}
output=${OUTPUT:-/busybox-new}
objects=${OBJECTS:-/busybox-obj}
object_list=${OBJECT_LIST:-/busybox-objects.args}
manifest=${SOURCES:-/busybox-riscv64.sources}
runner=${RUNNER:-}

for file in autoconf.h applet_tables.h usage_compressed.h NUM_APPLETS.h bbconfigopts_bz2.h; do
	test -f "$src/include/$file" || {
		echo "BUSYBOX: missing generated include/$file"
		exit 1
	}
done

rm -rf "$objects"
mkdir -p "$objects"
: > "$object_list"
cflags="-B$tcc_src -nostdinc -I$src/include -I$src/libbb -I$musl/obj/include -I$musl/include -I$musl/arch/riscv64 -I$musl/arch/generic -include $src/include/autoconf.h -D_GNU_SOURCE -DNDEBUG -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -D__GNUC__=2 -D__GNUC_MINOR__=95"
version_define='-DBB_VER="1.36.1"'
basename_define='-DKBUILD_BASENAME="busybox"'
modname_define='-DKBUILD_MODNAME="busybox"'

. "$manifest"
cd /
for dir in $directories; do
	mkdir -p "$objects/$dir"
	: > "$objects/$dir/.keep"
	set --
	for file in $sources; do
		case "$file" in "$dir"/*.c)
			rest=${file#"$dir"/}
			case "$rest" in */*) ;; *) set -- "$@" "$src/$file";; esac
		esac
	done
	echo "BUSYBOX: $dir"
	cd "$objects/$dir"
	"$cc" $cflags "$version_define" "$basename_define" "$modname_define" \
		-c "$@"
	for file in "$@"; do
		base=${file##*/}
		echo "$objects/$dir/${base%.c}.o" >> "$object_list"
	done
	cd /
done

echo 'BUSYBOX: linking'
"$cc" -B"$tcc_src" -static -nostdinc -nostdlib -o "$output" \
	"$musl/lib/crt1.o" "$musl/lib/crti.o" @"$object_list" \
	"$musl/lib/libc.a" "$tcc_src/libtcc1.a" "$musl/lib/crtn.o"
$runner "$output" echo 'BUSYBOX: runtime complete'
