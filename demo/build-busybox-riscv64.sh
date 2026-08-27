#!/bin/bash
# Clones real upstream busybox, applies patches/busybox-tcc-compat.patch
# (arch-independent -- see that patch's own history for why it's not
# per-arch), builds with our riscv64-targeting TCC. Needs
# build-musl-riscv64.sh to have run first.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
TCC="${TCC:-$HERE/../compiler/tcc}"
MUSL="${MUSL:-$HERE/../musl-riscv64}"
BUSYBOX_TAG=1_36_1
BUSYBOX_REPO=https://github.com/mirror/busybox.git

if [ ! -x "$TCC" ]; then
	echo "error: $TCC not found." >&2
	echo "Build it first: cd ../compiler && make TARGET=riscv64" >&2
	exit 1
fi
if [ ! -f "$MUSL/lib/libc.a" ]; then
	echo "error: $MUSL/lib/libc.a not found." >&2
	echo "Run ./build-musl-riscv64.sh first, or set \$MUSL." >&2
	exit 1
fi

echo "=== cloning real upstream busybox $BUSYBOX_TAG (unmodified) ==="
BB_DIR="$HERE/../busybox-riscv64"
rm -rf "$BB_DIR"
git clone -q --depth 1 --branch "$BUSYBOX_TAG" "$BUSYBOX_REPO" "$BB_DIR"
cd "$BB_DIR"
echo "HEAD: $(git rev-parse HEAD)"

echo ""
echo "=== applying patches/busybox-tcc-compat.patch ==="
git apply "$HERE/patches/busybox-tcc-compat.patch"

TCCDIR="$(dirname "$TCC")"
# Consumed by patches/busybox-tcc-compat.patch's scripts/trylink hunk,
# which needs musl's/libtcc1.a's startfiles but has no other way to
# learn where they live -- see that patch for why.
export MUSL_LIBDIR="$MUSL/lib"
export TCC_LIBDIR="$TCCDIR"
cat > /tmp/bb-wrapper-riscv64.sh << WRAPEOF
#!/bin/sh
exec "$TCC" -B"$TCCDIR" -I"$TCCDIR/include" \\
  -I"$MUSL/obj/include" -I"$MUSL/include" -I"$MUSL/arch/riscv64" -I"$MUSL/arch/generic" \\
  -L"$MUSL/lib" -nostdinc \\
  -D__GNUC__=2 -D__GNUC_MINOR__=95 \\
  "\$@"
WRAPEOF
chmod +x /tmp/bb-wrapper-riscv64.sh

echo ""
echo "=== building ==="
# .config isn't part of the patch (never was, for this arch): minimal
# applet set + CONFIG_LFS=y (musl always uses 64-bit off_t, matching a
# check busybox's own libbb.h already makes), generated fresh here the
# same way build-busybox-i386.sh's own minimal_config() does.
make allnoconfig >/dev/null 2>&1
for opt in ASH GUNZIP GZIP TAR BASENAME CAT CHMOD CHOWN CP CUT DIRNAME \
           ECHO ENV EXPR FALSE HEAD LN LS MKDIR MV PRINTF PWD RM RMDIR \
           SLEEP SORT TAIL TEST TOUCH TRUE UNIQ WC WHICH SED FIND GREP \
           MOUNT UMOUNT KILL PS LFS; do
	sed -i "s/# CONFIG_${opt} is not set/CONFIG_${opt}=y/" .config
done
yes "" | make oldconfig >/dev/null 2>&1
make CC=/tmp/bb-wrapper-riscv64.sh AR=ar STRIP=strip SKIP_STRIP=y \
	EXTRA_LDFLAGS="-static -nostdlib" -j1
echo "SUCCESS"

echo ""
echo "=== smoke test: real applets via qemu-riscv64-static ==="
if command -v qemu-riscv64-static >/dev/null; then
	mv busybox_unstripped busybox 2>/dev/null || true
	ln -sf busybox ash
	echo "  echo:"; qemu-riscv64-static ./busybox echo "  it works"
	echo "  ash (shell, arithmetic, pipes):"
	qemu-riscv64-static ./ash -c '
	echo "line1" > /tmp/bbsmoke.txt; echo "line2" >> /tmp/bbsmoke.txt
	echo "    $(cat /tmp/bbsmoke.txt | wc -l) lines, expr: $(expr 6 \* 7)"
	'
else
	echo "    (qemu-riscv64-static not found, skipping run -- binary built OK though)"
fi

echo ""
echo "busybox built at: $BB_DIR/busybox"
