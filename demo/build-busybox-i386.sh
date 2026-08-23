#!/bin/bash
# Clones the real upstream busybox (not a vendored copy) at the exact
# version this project tested against, tries building it under our
# TCC completely unpatched first -- which fails in two different ways
# -- then applies patches/busybox-tcc-compat.patch and adds the one
# fix that ISN'T a source patch (a compiler-flag spoof, explained
# below) to build successfully. See docs/busybox-findings.md for the
# full narrative; this script is the reproducible, minimal proof.
#
# Needs musl already built as a sibling: run build-musl.sh first, or
# set $MUSL to point at an existing build.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
TCC="${TCC:-$HERE/../compiler/tcc}"
MUSL="${MUSL:-$HERE/../musl-i386}"
BUSYBOX_TAG=1_36_1
BUSYBOX_REPO=https://github.com/mirror/busybox.git

if [ ! -x "$TCC" ]; then
	echo "error: $TCC not found." >&2
	echo "Build the tcc repo first (cd ../compiler && make TARGET=i386), or set \$TCC." >&2
	exit 1
fi
if [ ! -f "$MUSL/lib/libc.a" ]; then
	echo "error: $MUSL/lib/libc.a not found." >&2
	echo "Run ./build-musl.sh first, or set \$MUSL to an existing build." >&2
	exit 1
fi

echo "=== cloning real upstream busybox $BUSYBOX_TAG (unmodified) ==="
rm -rf "$HERE/busybox"
git clone -q --depth 1 --branch "$BUSYBOX_TAG" "$BUSYBOX_REPO" "$HERE/busybox"
cd "$HERE/busybox"
echo "HEAD: $(git rev-parse HEAD)"

TCCDIR="$(dirname "$TCC")"
plain_wrapper() {
	# no -D__GNUC__ spoof -- see attempt 2 for why that matters
	cat > /tmp/bb-wrapper-plain.sh << WRAPEOF
#!/bin/sh
exec "$TCC" -B"$TCCDIR" -I"$TCCDIR/include" \\
  -I"$MUSL/obj/include" -I"$MUSL/include" -I"$MUSL/arch/i386" -I"$MUSL/arch/generic" \\
  -L"$MUSL/lib" -nostdinc "\$@"
WRAPEOF
	chmod +x /tmp/bb-wrapper-plain.sh
}
patched_wrapper() {
	cat > /tmp/bb-wrapper-patched.sh << WRAPEOF
#!/bin/sh
exec "$TCC" -B"$TCCDIR" -I"$TCCDIR/include" \\
  -I"$MUSL/obj/include" -I"$MUSL/include" -I"$MUSL/arch/i386" -I"$MUSL/arch/generic" \\
  -L"$MUSL/lib" -nostdinc \\
  -D__GNUC__=2 -D__GNUC_MINOR__=95 \\
  "\$@"
WRAPEOF
	chmod +x /tmp/bb-wrapper-patched.sh
}

minimal_config() {
	make allnoconfig >/dev/null 2>&1
	for opt in ASH GUNZIP GZIP TAR BASENAME CAT CHMOD CHOWN CP CUT DIRNAME \
	           ECHO ENV EXPR FALSE HEAD LN LS MKDIR MV PRINTF PWD RM RMDIR \
	           SLEEP SORT TAIL TEST TOUCH TRUE UNIQ WC WHICH SED FIND GREP \
	           MOUNT UMOUNT KILL PS LFS; do
		sed -i "s/# CONFIG_${opt} is not set/CONFIG_${opt}=y/" .config
	done
	yes "" | make oldconfig >/dev/null 2>&1
}

echo ""
echo "=== attempt 1: unpatched upstream busybox, vanilla TCC (no flags) ==="
echo "    (this is expected to fail -- see below for what breaks)"
plain_wrapper
minimal_config
if make CC=/tmp/bb-wrapper-plain.sh AR=ar STRIP=strip SKIP_STRIP=y \
	EXTRA_LDFLAGS="-static -nostdlib" -j1 -k \
	> /tmp/busybox-unpatched-build.log 2>&1; then
	echo "    unexpectedly succeeded?!"
else
	echo "    FAILED, as expected. First real error:"
	grep -m1 -E "error:|invalid option" /tmp/busybox-unpatched-build.log | sed 's/^/    /'
	echo "    (that's Kbuild's GCC-specific -Wp,-MD,file dependency-tracking"
	echo "     syntax; TCC has its own -MD/-MF flags but doesn't parse -Wp,'s"
	echo "     comma-splitting convention at all -- fixed in the patch)"
	echo "    (full log: /tmp/busybox-unpatched-build.log)"
fi

echo ""
echo "=== attempt 1b: same, but only fixing the -Wp,-MD build-system issue ==="
echo "    (isolates the SEPARATE __attribute__ gap from the build-system one)"
git stash -q 2>/dev/null || true
sed -i 's/-Wp,-MD,\$(depfile)/-MD -MF $(depfile)/' scripts/Makefile.lib scripts/Makefile.host
if make CC=/tmp/bb-wrapper-plain.sh AR=ar STRIP=strip SKIP_STRIP=y \
	EXTRA_LDFLAGS="-static -nostdlib" -j1 -k \
	> /tmp/busybox-partial-build.log 2>&1; then
	echo "    unexpectedly succeeded?!"
else
	echo "    STILL FAILS. First real compile error:"
	grep -m1 "error:" /tmp/busybox-partial-build.log | sed 's/^/    /'
	echo "    TCC predefines neither __GNUC__ nor __GNUC_MINOR__. busybox's"
	echo "    platform.h has a portability shim: '#if !__GNUC_PREREQ(2,7) ..."
	echo "    #define __attribute__(x)' -- meant for compilers with NO"
	echo "    __attribute__ support at all, but it fires for TCC too (which"
	echo "    supports __attribute__ fine, it just doesn't self-identify as"
	echo "    GCC), silently deleting every attribute in the codebase."
fi
git checkout -q -- scripts/Makefile.lib scripts/Makefile.host

echo ""
echo "=== applying patches/busybox-tcc-compat.patch ==="
git checkout -q -- . 2>/dev/null || true
git clean -qfdx 2>/dev/null || true
git apply "$HERE/patches/busybox-tcc-compat.patch"
echo "    applied: .config (minimal applet set + CONFIG_LFS=y -- musl always"
echo "    uses 64-bit off_t, matching a check busybox's own libbb.h already"
echo "    makes), the -Wp,-MD fix, and three TCC linker-limitation fixes in"
echo "    scripts/trylink (no --start-group support, an EXTRA_LDFLAGS leak"
echo "    into intermediate partial-link steps, and unsupported diagnostic-"
echo "    only linker flags). Full detail: docs/busybox-findings.md."

echo ""
echo "=== attempt 2: patched busybox + GCC-version-spoofed TCC ==="
echo "    (-D__GNUC__=2 -D__GNUC_MINOR__=95 -- same technique Clang uses,"
echo "     for the same reason. Deliberately conservative: high enough to"
echo "     satisfy __GNUC_PREREQ(2,7) and unlock __attribute__, but below"
echo "     __GNUC_PREREQ(3,0) and (4,1), which gate regparm/stdcall and a"
echo "     GCC visibility pragma TCC confirmed it does NOT support)"
patched_wrapper
# Not using the patch's own build.sh here: it assumes busybox sits as
# a sibling of tcc/musl, which is a fine assumption for someone using
# the patched tree standalone (see its own comments) but doesn't match
# this script's layout (busybox cloned *under* demo-tcc-gaps/). Same
# build command either way, just with this script's own path vars.
if make CC=/tmp/bb-wrapper-patched.sh AR=ar STRIP=strip SKIP_STRIP=y \
	EXTRA_LDFLAGS="-static -nostdlib" -j1 \
	> /tmp/busybox-patched-build.log 2>&1; then
	echo "    SUCCESS"
else
	echo "    FAILED -- this shouldn't happen. See /tmp/busybox-patched-build.log"
	tail -30 /tmp/busybox-patched-build.log
	exit 1
fi

echo ""
echo "=== smoke test: real applets via qemu-i386-static ==="
if command -v qemu-i386-static >/dev/null; then
	echo "  echo:"; qemu-i386-static ./busybox_unstripped echo "  it works"
	ln -sf busybox_unstripped ash
	echo "  ash (shell, arithmetic, pipes):"
	qemu-i386-static ./ash -c '
	echo "line1" > /tmp/bbsmoke.txt; echo "line2" >> /tmp/bbsmoke.txt
	echo "    $(cat /tmp/bbsmoke.txt | wc -l) lines, expr: $(expr 6 \* 7)"
	'
else
	echo "    (qemu-i386-static not found, skipping run -- binary built OK though)"
fi

echo ""
echo "busybox built at: $HERE/busybox/busybox_unstripped"
