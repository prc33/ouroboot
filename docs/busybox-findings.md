# busybox: ash + ~40 coreutils building and running under TCC + musl

## Verdict: yes, with three build-system patches and one compiler-flag fix

A minimal, targeted busybox config (ash plus core utilities -- not the
full 350+-applet `defconfig`, most of which isn't in scope yet: no
networking, no syslog) builds cleanly under our stripped TCC and the
patched musl from the earlier spike, and runs correctly under
`qemu-i386-static`. Verified with a real multi-utility pipeline through
`ash` (mkdir, redirection, `cat`, `wc -l`, `sort -r`, `grep`, `sed`,
`find`, `expr`, `basename`/`dirname`, piped chains), plus `argv[0]`-based
applet dispatch via symlinks -- the actual real-world invocation style,
not just direct calls.

## Method

Same shape as the musl spike: `apt-get source busybox` (Ubuntu 24.04
universe), minimal config via `scripts/config`-style flag flipping plus
`oldconfig`, iterate on each distinct build failure until clean.

One methodological note worth carrying forward: the *pristine* baseline
for diffing was Ubuntu's own patched source (post `dpkg-source -x`,
which applies ~13 Debian/Ubuntu patches -- CVE fixes, platform
compatibility, an awk precedence fix), not the bare upstream tarball.
An early attempt at diffing against pure upstream produced a huge,
misleading diff dominated by Ubuntu's own pre-existing patches,
falsely attributing them to us. Diffing from the actual starting point
(what `apt-get source` really gives you) is what makes the resulting
patch an accurate record of what changed.

## Findings, in the order encountered

### 1. `-Wp,-MD,$(depfile)` — a build-system flag mismatch, not a compiler issue
Kbuild's automatic dependency generation passes GCC's comma-joined
`-Wp,-MD,file` syntax. TCC has its own `-MD`/`-MF file` flags but
doesn't parse `-Wp,`'s comma-splitting convention at all. **Fix:**
`scripts/Makefile.lib`/`scripts/Makefile.host`, `-Wp,-MD,$(depfile)` →
`-MD -MF $(depfile)`.

### 2. `CONFIG_LFS` — busybox's own assertion catching a real ABI fact, not a bug
A compile-time size assertion (`BUILD_BUG_ON(sizeof(off_t) ==
sizeof(uoff_t))`) failed. musl always uses a 64-bit `off_t` (a
deliberate musl design choice -- unlike glibc, no
`_FILE_OFFSET_BITS=64` needed or honored), but busybox's default
non-LFS config assumes 32-bit. busybox's own source comment names this
exact scenario. **Fix:** enable `CONFIG_LFS`, exactly as recommended.

### 3. The `__GNUC_PREREQ` / `__attribute__` neutering — the hard one

A packed-struct-in-union compile-time size assertion failed in
`archival/libarchive/decompress_gunzip.c`. Every isolated reproduction
using the identical macro text compiled correctly, which ruled out a
general TCC bug with packed attributes in nested contexts. Bisected by
inserting a probe struct at successive line numbers -- first narrowing
which of two `#include`s was responsible, then within that header,
then within the header *it* includes -- until it localized to a
portability shim in `platform.h`:

```c
#if !__GNUC_PREREQ(2,7)
# ifndef __attribute__
#  define __attribute__(x)
# endif
#endif
```

TCC predefines neither `__GNUC__` nor `__GNUC_MINOR__` at all
(confirmed directly: `tcc -E -dM` on empty input shows neither), so
`__GNUC_PREREQ` always evaluates to its no-both-macros-defined
fallback of `0`. This activates the shim meant for compilers that
*genuinely* don't support `__attribute__` — silently deleting every
`__attribute__` usage in the whole codebase, even though TCC's actual
support for `__attribute__((packed))` is fine (confirmed repeatedly in
isolation). **Not a TCC bug** — busybox's own capability-detection
logic misfiring on a compiler that doesn't self-identify as
GCC-derived.

**Fix:** `-D__GNUC__=2 -D__GNUC_MINOR__=95` on the compile line — the
same technique Clang uses, for the same reason. The specific version
was chosen deliberately, not just "something recent": 2.95 satisfies
`__GNUC_PREREQ(2,7)` (unlocking `__attribute__`) while staying below
`__GNUC_PREREQ(3,0)` (which gates `FAST_FUNC`'s `regparm`/`stdcall`
attributes, unverified on TCC) and `__GNUC_PREREQ(4,1)` (which gates a
`_Pragma("GCC visibility push(hidden)")` — confirmed separately, in
isolation, that TCC does **not** support this pragma at all, reproducing
the identical "identifier expected" error). Spoofing exactly enough
capability to fix the one real problem, deliberately not more.

After this fix, every single `.c` file in the configured build
compiled cleanly on the first subsequent attempt.

### 4–6. Three linker-stage issues, all genuine TCC linker limitations

**No `--start-group`/`--end-group`.** TCC's linker is single-pass with
no archive-cycle rescanning — confirmed as a hard error, not a silent
no-op. busybox's per-directory static archives (`libbb/lib.a` etc) are
listed in directory-alphabetical order, not dependency order, so
symbols needed by earlier-listed archives but defined in
later-listed ones don't resolve in one pass. **Fix:** relist busybox's
own archives a second time (a poor-man's version of what
`--start-group` does under the hood — repeated rescanning until no
more symbols resolve), then relist `libc.a` once more after that,
since symbols only pulled in during the second busybox pass can still
need libc functions that are already behind the scan position.

**`EXTRA_LDFLAGS` leaking into intermediate partial links.** The
musl/TCC startfiles were first supplied via make's `EXTRA_LDFLAGS`,
which turned out to also apply to every per-directory `ld -r`
partial-link step (building each directory's `built-in.o`), not just
the real final link. Confirmed directly by inspecting
`applets/.built-in.o.cmd` and finding a full copy of
`_start`/`__libc_start_main`/`memcpy` already baked into
`applets/built-in.o`, which then collided with the same symbols at
the real final link ("defined twice"). **Fix:** moved the startfiles
out of the make-level flag entirely and into `scripts/trylink`
directly, which is only ever invoked for the one real link.

**Diagnostic-only linker flags.** `-Wl,--warn-common`, `-Wl,-Map,...`,
`-Wl,--verbose` — none affect the produced binary, all three
unsupported by TCC's linker. **Fix:** emptied the `INFO_OPTS` helper
that supplied them.

## What's verified, concretely

`ash -c` running a real script: file redirection, `cat`, `wc -l`,
`sort -r`, `grep`, `sed` substitution, `find`, `expr` arithmetic,
`basename`/`dirname`, and a piped chain (`cat | grep | wc -l`) — every
step producing correct output. Applet dispatch via `argv[0]` (symlinks
named after each applet, e.g. `ash`, `ls`, `sed`) confirmed working,
which is the actual mechanism real systems use, not just direct
`./busybox <applet>` invocation.

## What's not done

Not yet wired into the kernel — busybox needs `fork`/`exec` and a real
filesystem (ramfs/VFS), neither of which exist yet (kernel P6). This
checkpoint is the same shape as the original musl/TCC spike: prove the
*toolchain* produces correct, runnable binaries, as a prerequisite
before the harder work of running them under our own kernel. Networking
applets, syslog, and the full applet set remain out of the configured
scope for now, matching the project's existing scope boundary.
