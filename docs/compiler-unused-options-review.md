# Compiler review: unused compile-time options and command-line arguments

Scope: `compiler/` only, read-only review, no source changed. Goal stated by the
request this answers: **keep all three backends (i386, riscv64, wasm32)** —
nothing here proposes cutting a target or any standard-C language behavior.
This is specifically about `#ifdef`-gated code and command-line options that
none of this project's *own* builds ever exercise.

## Methodology

"Never used by our self-hosting usecases/builds" was checked against every
real invocation of `tcc`/`wasm32-tcc` this repository's own tooling actually
performs, not against upstream TCC's full feature set in the abstract:

- `compiler/Makefile` itself (`tcc`/`wasm32-tcc`/`libtcc1.a`/`stage1`/`selfcheck`
  targets — the host bootstrap, runtime-library build, and the riscv64-in-QEMU
  self-hosting proof).
- `kernel/Makefile` (both i386 and riscv64 kernel builds) and
  `kernel/test/build-selfhost-initrd.sh` / `kernel/test/selfhost.sh` (the
  actual **in-kernel** self-hosting build this project's closure milestone
  depends on).
- `demo/build-musl-{i386,riscv64}.sh` and `demo/build-busybox-{i386,riscv64}.sh`,
  plus `demo/patches/*.patch` (which frequently exist specifically to remove a
  flag/construct TCC can't handle — a patch removing something is itself
  evidence that thing is never actually reached).
- `emulator/Makefile` and `compiler/tests/wasm32/Makefile` (the wasm32 backend's
  own consumers).
- musl-riscv64's own `config.mak`, already generated on disk in this sandbox by
  a real `./configure` run — this is the actual, concrete flag set musl's
  build probed for and uses, not a guess about what a libc's build might do.

Confidence varies by source: kernel/compiler/emulator invocations and musl's
`config.mak` are **directly observed, high confidence**. BusyBox's Kbuild is
far more dynamic (flags assembled from `.config` + generated fragments at
build time, not sitting in one static file) — those findings are **medium
confidence**, corroborated by what `demo/patches/busybox-*-tcc-compat.patch`
had to remove, and are flagged as such below.

Line counts for `#ifdef` blocks are a single-pass block scanner (matches
`#ifdef`/`#if defined(...)` against a macro list, tracks nesting depth, sums
line extents of blocks whose condition references *only* dead macros in a
non-negated way — a block also reachable by a live target macro, e.g.
`#if defined(TCC_TARGET_I386) || defined(TCC_TARGET_X86_64)`, is correctly
excluded). This is an estimate, not a byte-exact figure — treat the numbers as
"tens of lines" precision, not exact.

## Part 1 — compile-time options (`#ifdef`)

### 1a. Dead target/platform macros — ~1,860 lines, zero risk to delete

These can **never** be true in any build this project produces, for a reason
stronger than "unused": the backend source files they'd select don't exist in
this tree at all (already removed in an earlier pass — confirmed by listing
`compiler/*-gen.c`: only `i386-gen.c`, `riscv64-gen.c`, `wasm-gen.c` exist).
Every reference below is compiled-out dead code in literally every
`TARGET=` this Makefile supports.

| Macro | Why it's dead here | ~Lines | Main files |
|---|---|---:|---|
| `TCC_TARGET_X86_64` | No `x86_64-gen.c` in this tree; upstream's shared i386/x86_64 codegen paths were never fully separated | ~610 (largest single contributor, spread across many files) | `tccelf.c`, `tcc.h`, `tccgen.c`, `i386-asm.c`, `libtcc.c` |
| `TCC_TARGET_PE` (+ `_WIN32`, `_MSC_VER`, `LIBTCC_AS_DLL`) | Windows/PE output was never a target; `-impdef` (PE import-def tool) is already correctly excluded from the option table for every real target | ~520 | `tccelf.c`, `libtcc.c`, `tcc.c`, `i386-asm.c` |
| `TCC_TARGET_ARM` / `TCC_ARM_EABI` / `TCC_ARM_VFP` / `TCC_ARM_HARDFLOAT` | No `arm-gen.c`; `-mfloat-abi` option-table entry is *already* `#ifdef TCC_TARGET_ARM`-gated, so it doesn't even exist as a recognized flag today | ~330 | `tcc.h`, `tccgen.c`, `tccelf.c` |
| `TCC_TARGET_ARM64` | No `arm64-gen.c` | ~140 | `tcc.h`, `tccelf.c` |
| `TCC_TARGET_MACHO` (+ `__APPLE__`) | No Mach-O writer in this tree | ~130 | `tccelf.c`, `libtcc.c` |
| `TCC_TARGET_C67` | No `c67-gen.c`; this is upstream's most obscure/already-half-dead target | ~60 | `tcc.h`, `tccelf.c` |
| `TCC_TARGET_COFF` | No COFF object writer | ~40 | `tccelf.c` |
| `__FreeBSD__` / `__FreeBSD_kernel__` / `__NetBSD__` / `__OpenBSD__` / `__GNU__` | Host-OS conditionals for BSD-family/Hurd hosts; this project only ever hosts the compiler on Linux (build/CI is Linux, and the kernel/emulator targets are the actual products) | ~30 | `libtcc.c`, `tcc.c` |

**~1,860 lines total.** All of it lives in files that stay (`tcc.h`, `tccelf.c`,
`libtcc.c`, `tccgen.c`, `i386-asm.c`, `tcc.c`, `tccpp.c`, `tcctok.h`,
`tccasm.c`, `i386-gen.c`, `i386-tok.h`) — this is textual clutter distributed
through files this project actively maintains, not a handful of files that
could just be deleted wholesale. Removing it is a mechanical "delete every
`#ifdef DEAD_MACRO ... #endif` block" pass per file, safe because none of
these macros can ever become true without adding a fourth backend (explicitly
out of scope here).

### 1b. Debug-instrumentation macros — ~136 lines, zero risk to delete

`TAL_DEBUG`, `TAL_INFO`, `ASM_DEBUG`, `MEM_DEBUG`, `INC_DEBUG`, `BF_DEBUG`,
`PARSE_DEBUG`, `SYM_DEBUG`, `DEBUG_RELOC` gate `fprintf`-to-stderr tracing
upstream TCC's own developers use (allocator tracing, include-path tracing,
bitfield-layout tracing, etc.). None are defined by `config.h`, `Makefile`, or
anywhere else in this project — confirmed by grep, not inferred. They're not
reachable via any documented flag either (they're compiled-in-or-not, not
runtime-selectable). Concentrated in `tccpp.c` (74 lines), `tccgen.c` (24),
`i386-asm.c` (15), `libtcc.c` (11), `tccasm.c` (6).

### 1c. Feature macros never enabled — ~830 lines across two independent decisions already made

| Macro | Status | ~Lines | Note |
|---|---|---:|---|
| `CONFIG_TCC_BCHECK` | Never defined anywhere in this project | ~170 (`tccelf.c`, `tccgen.c`, `i386-gen.c`, `riscv64-gen.c`, `tcc.h`, `tcctok.h`) | Runtime bounds-checking instrumentation. The **`-b`/`-bt`/`-ba` option-table entries already fall through to "unsupported option" today** since their `switch` cases live inside this same `#ifdef` — but the table entries themselves (`{"b", ...}`, `{"bt", ...}`) are still unconditionally present, so removing the macro's dead body without also removing those three table rows leaves three flags that parse successfully and then do nothing useful. See §2 below. |
| `CONFIG_TCC_BACKTRACE` | Never defined | (included above) | Runtime stack-backtrace support for the bounds checker; only meaningful together with `CONFIG_TCC_BCHECK` |
| `TCC_IS_NATIVE` | Already permanently undefined by design (an earlier pass replaced the whole `#if defined _WIN32 == defined TCC_TARGET_PE` block with a comment explaining it's intentionally dead — see `tcc.h`'s own history) | small, residual | A few `#ifdef TCC_IS_NATIVE` references remain as truly-dead code now; low line count, already effectively handled, listed for completeness |
| `CONFIG_TCC_ELFINTERP` | *Defined* (`config.h` sets it to a real path string), but its actual effect — emitting a `PT_INTERP` section naming a dynamic-linker path — only matters for **dynamically linked** executables | ~660 combined with `tccelf.c`'s dynamic-symbol/PLT/GOT machinery (a much bigger, harder-to-isolate case; see §3) | Every real output this project produces is `-static -nostdlib` (musl, BusyBox, TCC's own self-hosted stage1/stage2, the kernel itself). No script anywhere in this repo ever omits `-static`. |

## Part 2 — command-line options

### 2a. Confirmed used — keep, no action

`-B`, `-I`, `-D`, `-L`, `-l`, `-c`, `-o`, `-v`, `-static`, `-nostdinc`,
`-nostdlib`, `-ar`, `-MD`, `-MF`, `-Wl,`, `-std` (see the caveat in §2d),
`-w`, `-pipe` (accepted, genuinely reaches TCC via musl's own `CFLAGS_AUTO`,
but is a documented no-op — see §2d).

`-MD`/`-MF` deserve a specific note: `demo/build-busybox-{i386,riscv64}.sh`
carries a patch (`sed -i 's/-Wp,-MD,\$(depfile)/-MD -MF $(depfile)/'`)
specifically *because* BusyBox's Kbuild defaults to GCC's `-Wp,-MD,file`
comma-joined syntax, which TCC's `-Wp,` handling (a generic "re-parse this as
another option" passthrough) can't split. The comma-joined form is worked
around, never made to work — see §2d.

### 2b. Already correctly unreachable — no action needed, listed for completeness

- `-impdef` — table entry is `#ifdef TCC_TARGET_PE`-gated; doesn't parse as a
  recognized option for any of the three kept targets already.
- `-mfloat-abi` — table entry is `#ifdef TCC_TARGET_ARM`-gated; same situation.

These are good examples of the right pattern (§1's fix, applied to the option
table specifically) already being followed correctly in two places — the rest
of §1's dead macros should get the same treatment where they also touch the
option table, not just the `#ifdef` bodies deep in the option-handling switch.

### 2c. Never invoked by anything in this project — candidates for removal

None of these appear in any Makefile, shell script, or musl's real
`config.mak` anywhere in this repository:

| Option | Current behavior | Why it's dead here |
|---|---|---|
| `-run` | Already hard-errors (`tcc_error("-run is not available...")`) — `tccrun.c` was deleted in an earlier pass | The *table entry* and `TCC_OPTION_run` enum/dispatch machinery remain, just to produce that error. Could be deleted from the table entirely (falling through to the generic "unrecognized option" error) with no behavior change this project would ever observe. |
| `-shared` / `-soname` | Sets `TCC_OUTPUT_DLL` / stores a soname string | No script anywhere in this repo ever builds a `.so`; every output is static. `TCC_OUTPUT_DLL` as an output-type value also feeds into `tccelf.c`'s dynamic-linking object-writing path (see §3 — this is really a #1c-style feature question, not just an option question) |
| `-b`, `-bt`, `-ba` | Table entries present; switch cases compiled out (`CONFIG_TCC_BCHECK`/`CONFIG_TCC_BACKTRACE` unset) → falls through to "unsupported option" | Bounds-checking/backtrace was never enabled (§1c). These three table rows are the option-table half of that same decision. |
| `-r` | Merges multiple inputs into one relocatable `.o` | Not used by musl (produces `.a` archives via `-ar`, not `-r`-merged objects), not used by BusyBox, not used by this project's own kernel/self-hosting builds |
| `-x` (language override: `-xc`/`-xassembler`/...) | Forces input-file-type detection | Every build in this repo relies on extension-based auto-detection (`.c`, `.S`) |
| `-rdynamic` | Exports all symbols for dynamic linking | Static-only outputs, same reasoning as `-shared` |
| `--param` | GCC compatibility no-op passthrough | Not observed anywhere; GCC-specific tuning knob with no TCC equivalent behavior |
| `-pedantic` | Sets a warning flag with no corresponding enforcement found wired up | Not observed anywhere |
| `-pthread` | Sets `option_pthread` (affects link-time `-lpthread` behavior) | Every build here is single-threaded static (musl static, BusyBox static, the kernel's own userspace); not observed in musl's `config.mak` or this project's own scripts |
| `-bench` | Prints compile-time statistics | Not observed; a developer convenience flag, not a build-pipeline dependency |
| `-print-search-dirs` | Prints TCC's configured search paths and exits | Not observed |
| `-dumpversion` | Prints `TCC_VERSION` and exits | Not observed (this project's own `-v` self-identification check in `selfcheck`/`selfhost.sh` uses `-v`, not `-dumpversion`) |
| `-C` | "Keep comments" — already `/* ignored */` (a no-op today, upstream removed real behavior) | Confirms it's already inert; safe to delete the table entry too |
| `-E` (preprocess-only) | Sets `TCC_OUTPUT_PREPROCESS` | Not observed — no script in this repo ever preprocesses without compiling |
| `-P` | Preprocessor line-marker control | Not observed |
| `-isystem` / `-include` | System-include-path / forced-include-file options | Not observed in this project's own invocations (musl/BusyBox use plain `-I`) |

**Medium-confidence caveat**: several of these (`-pthread` in particular)
could plausibly be probed-and-discarded by BusyBox's Kbuild the way `-pipe`
is by musl's `./configure`, without that probe's result ever surfacing in a
static file this review could grep. Treat the BusyBox-only-sourced entries
above as "not observed in this project's available build artifacts," not
"proven impossible to reach."

### 2d. Used, but worth documenting rather than deleting

Two real oddities, both about correctness/honesty of behavior rather than
line-count reduction — flagged because they're the kind of thing "looks
broader than it is" (this project's own stated engineering value):

- **`-std=` only ever matches the literal string `=c11`.** Every real build
  in this repo that passes `-std` passes `-std=c99` (musl's own
  `CFLAGS_C99FSE`). TCC's handler does `if (strcmp(optarg, "=c11") == 0) ...`
  with no `else` — meaning `-std=c99` is silently accepted and has *zero*
  effect, not even a warning. The flag is genuinely "used" by the actual
  build, but the one value the parser branches on is never the value it
  receives in practice.
- **`-Wp,` never handles its comma-joined multi-argument form.** BusyBox's
  default Kbuild flags (`-Wp,-MD,$(depfile)`) are patched away specifically
  because TCC's `-Wp,` reparse-as-option logic splits on the first comma only
  well enough for a single trailing option, not GCC's `-Wp,opt1,opt2,...`
  convention. Kept working today only because the patch avoids ever sending
  TCC that syntax, not because TCC handles it.

Neither is a deletion candidate — `-std` must keep accepting *something* or
musl's build breaks, and `-Wp,` genuinely is used in its single-argument form
elsewhere. Both are candidates for either implementing the missing behavior
properly or documenting the limitation next to the option table, so a future
reader doesn't assume more coverage than exists.

## Part 3 — the bigger, harder-to-isolate case: static-only linking

`-shared`/`-soname`/`-rdynamic` (§2c) are the command-line surface of a
larger design fact: **this project never produces a dynamically linked
output, anywhere, in any build.** musl static, BusyBox static
(`EXTRA_LDFLAGS="-static -nostdlib"`), TCC's own self-hosted stage1 and the
in-kernel stage2 (`-static -nostdlib` in both `compiler/Makefile`'s `stage1:`
target and `kernel/test/selfhost.sh`) — every single link this repository's
tooling performs is static. `tccelf.c`'s dynamic-symbol-table, PLT/GOT
relocation, `.dynamic` section, and `CONFIG_TCC_ELFINTERP`-driven `PT_INTERP`
code paths (all reachable only from `TCC_OUTPUT_DLL` or a non-static
`TCC_OUTPUT_EXE`) are consequently as dead as anything in §1c, but they're
harder to isolate cleanly: unlike `TCC_TARGET_ARM`, this isn't a single
preprocessor macro — it's a runtime branch on `s->output_type`/
`s->static_link` threaded through relocation-emission code that's shared with
the static-linking path it needs to stay correct. Flagging this as a **real,
larger-than-`#ifdef`-scope simplification opportunity** rather than
attempting a line count here: a genuine "static-only linker" reduction of
`tccelf.c` would need its own focused pass (likely a few hundred lines, given
`tccelf.c` is 3,658 lines total and dynamic-linking-only logic is a real but
minority fraction of it), not a mechanical `#ifdef` deletion.

## Summary

| Category | Est. lines | Confidence | Action |
|---|---:|---|---|
| Dead target/platform `#ifdef`s (§1a) | ~1,860 | High | Delete — these macros can never be true with only 3 backends kept |
| Debug-instrumentation `#ifdef`s (§1b) | ~136 | High | Delete — never defined, no runtime toggle exists |
| Bounds-check/backtrace `#ifdef`s (§1c) | ~170 | High | Delete, together with the `-b`/`-bt`/`-ba` option-table rows (§2c) |
| Dead/never-invoked CLI options (§2c) | option-table + dispatch overhead only, not large in isolation | Medium–high (varies per flag) | Remove table entries; let them fall through to the existing "unrecognized option" error, same pattern already correctly used for `-impdef`/`-mfloat-abi` |
| `-std=`/`-Wp,` gaps (§2d) | n/a | High (behavior confirmed) | Document or fix; not a deletion |
| Static-only linking dead code in `tccelf.c` (§3) | Not estimated — needs its own pass | Medium | Larger follow-up, out of scope for a line-count table |

**Total confidently-deletable `#ifdef` surface: ~2,160 lines**, entirely
within files this project keeps and actively maintains (no file becomes
emptyable outright — this is textual-clutter removal distributed across
`tcc.h`, `tccelf.c`, `libtcc.c`, `tccgen.c`, `tccpp.c`, `tcctok.h`,
`tccasm.c`, `i386-asm.c`, `i386-gen.c`, `i386-tok.h`, `tcc.c`), plus a small,
well-identified set of command-line options with zero remaining callers in
this project's own build pipeline. Nothing above touches the C-language
front end (`tccgen.c`'s parser/type-system logic, `tccpp.c`'s preprocessor)
or any of the three kept backends' actual code generation.
