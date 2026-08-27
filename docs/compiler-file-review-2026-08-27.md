# `compiler/` — per-file review and complexity analysis, 2026-08-27

Scope: all 33,115 tracked lines under `compiler/`, reviewed file by file, with
each file assessed for **algorithmic** simplifications rather than cosmetic
ones. Written as a follow-on to `repo-review-2026-08-26.md`, whose §2 and §3
(the only two compiler findings it raised) are both already done — this goes
a level deeper, into the code those sections left standing.

Constraint carried throughout: **standard C compatibility is non-negotiable.**
Nothing below proposes dropping a C language feature. Where a proposal touches
something a real build depends on (musl's inline asm, BusyBox's `__attribute__`
usage), that dependency is stated and the proposal is scoped around it.

Every number here was measured. Method is stated inline at each claim; the
tooling is reproducible from the commands in this document's own history.

---

## The headline, before the detail

**TCC's algorithms are, with a handful of exceptions, already good.** This is
the most important finding and it cuts against the framing of the question.
Line count here is not the residue of naive algorithms — symbol lookup is
already O(1), the ELF hash table already doubles, the parser is already a
single-pass recursive-descent design with no IR to speak of. A profile of a
real self-hosting workload (TCC compiling its own 11 translation units and
linking against musl's `libc.a`) spends **0.03 seconds** of sampled CPU, spread
thinly across the lexer. There is no hot spot to fix.

The 33k lines come from three structural facts, in descending order of size:

1. **The tree holds three backends.** No single build ever compiles more than
   **18,471** lines (measured below). The other ~14k is the two backends this
   build isn't using.
2. **TCC contains its own assembler and linker.** That is precisely the part
   that small C compilers omit, and precisely the part this project cannot
   omit — see "Why the small-compiler comparison doesn't transfer" below.
3. **Breadth of C and GNU-extension coverage**, which is the thing we are not
   allowed to trade away.

So the realistic target is not "get to 10k". It is: remove what is provably
dead (verified below: ~800 lines, zero risk), and reduce the *structural*
complexity of a short list of genuinely overgrown functions — which improves
maintainability without moving the line count much at all.

---

## Method

| What | How it was measured |
|---|---|
| Per-file line counts | `git ls-files \| xargs wc -l`; code/comment/blank split by a per-line classifier tracking block-comment state |
| **Lines that actually compile, per target** | `gcc -E` on the real translation-unit set for each `TCC_TARGET_*`, attributing every surviving line back to its origin file via `# line "file"` markers, deduplicated per file (so a header included by 11 `.c` files counts once) |
| Dead code | `-ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--print-gc-sections` — linker-*proven* unreachability, not inference |
| Preprocessor-dead regions | `unifdef` against the set of macros never defined by any of the three real builds, then **rebuilt and diffed byte-for-byte** |
| Cyclomatic complexity | decision points (`if`/`for`/`while`/`case`/`goto`/`&&`/`\|\|`/`?`) + 1, per function, bounded at the next top-level definition |
| Hot paths | `gcc -pg` build, real self-hosting compile+link of the whole compiler against musl, `gprof` flat profile |

---

## Where the lines are, architecturally

Grouping by *job* rather than by directory, since that is what explains the size:

| Lines | % | Job |
|---:|---:|---|
| 12,208 | 38% | **C front end** — preprocessor, parser, type system, IR (`tccpp.c`, `tccgen.c`, `tcctok.h`) |
| 4,771 | 14% | **Linker + ELF** — because there is no external `ld` (`tccelf.c`, `elf.h`, `stab.*`) |
| 3,683 | 11% | **Driver, options, internal header** (`tcc.c`, `libtcc.c`, `tcc.h`, `libtcc.h`) |
| 3,537 | 11% | **Assembler** — because there is no external `as` (`tccasm.c`, `i386/i386-asm.*`, `i386/i386-tok.h`, `risc/riscv64-asm.c`) |
| 3,168 | 9% | Backend: wasm32 |
| 1,887 | 5% | Backend: riscv64 |
| 1,555 | 4% | Backend: i386 |
| 1,142 | 3% | Misc tools + runtime (`tcctools.c`, `lib/libtcc1.c`, softquad, `alloca86.S`) |
| **31,951** | | (excludes `COPYING`, `RELICENSING`, `Makefile`, `include/`, `tests/`) |

### The number that reframes everything

Lines that survive preprocessing and actually reach the compiler, per target:

| Target | Lines actually compiled |
|---|---:|
| i386 | **18,471** |
| riscv64 | **17,271** |
| wasm32 | **17,082** |

A single-target tree would be roughly 25–27k lines. **The three-backend
structure is the single largest contributor to the 33k figure, and it is a
deliberate project goal, not an accident.** Any honest accounting of "the
compiler is 33k lines" should carry this caveat.

---

## Per-file breakdown

`src` = non-blank source lines. `i386`/`rv64`/`wasm` = lines from that file
that survive preprocessing for that target (`0` = not part of that build).
For `.c` files the ratio is a meaningful dead-code signal; **for headers it is
not** — `#define`s are consumed by the preprocessor and never appear in its
output, so a header's macros are invisible to this measurement. Headers are
assessed separately by symbol reference.

| File | src | i386 | rv64 | wasm | Verdict |
|---|---:|---:|---:|---:|---|
| `tccgen.c` | 7,403 | 5,984 | 6,052 | 5,960 | Core. Genuinely large; see per-file notes |
| `tccpp.c` | 3,654 | 3,165 | 3,166 | 3,161 | Core, near-fully shared, algorithmically sound |
| `tccelf.c` | 3,080 | 2,497 | 2,491 | 2,473 | Contains the only sizeable *feature* dead weight |
| `wasm/tccwasm.c` | 1,714 | 0 | 0 | 1,536 | Highest structural complexity in the tree |
| `libtcc.c` | 1,602 | 1,237 | 1,243 | 1,233 | Driver; table-driven opportunity |
| `risc/riscv64-gen.c` | 1,451 | 0 | 1,174 | 0 | Backend; fine |
| `i386/i386-asm.c` | 1,357 | 1,110 | 0 | 0 | One real algorithmic wart |
| `tcc.h` | 1,314 | 626 | 617 | 586 | Monolithic internal header — coupling issue |
| `tccasm.c` | 1,219 | 1,086 | 1,074 | 0 | Directive dispatch; mild |
| `wasm/wasm-gen.c` | 970 | 0 | 0 | 926 | Backend; fine |
| `i386/i386-gen.c` | 956 | 729 | 0 | 0 | Backend; fine |
| `elf.h` | 797 | 322 | 322 | 322 | Already pruned once; 38 macros still unreferenced |
| `i386/i386-asm.h` | 422 | 397 | 0 | 0 | Opcode **table** — data, not complexity |
| `risc/riscv64-link.c` | 320 | 0 | 286 | 0 | Fine |
| `tcctok.h` | 293 | 200 | 232 | 196 | Token table; 10 unreferenced |
| `i386/i386-pair.c` | 289 | 252 | 0 | 0 | 64-bit-on-32-bit helpers; fine |
| `tcctools.c` | 286 | 243 | 243 | 243 | `ar` + makedeps; both used |
| `tcc.c` | 276 | 242 | 242 | 241 | Thin `main()`; fine |
| `i386/i386-link.c` | 192 | 148 | 0 | 0 | Fine |
| `i386/i386-tok.h` | 190 | 167 | 0 | 0 | Token table |
| `stab.def` | 186 | 42 | 42 | 42 | **Debug-only; unreachable in practice** |
| `wasm/wasm-backend.h` | 117 | 0 | 0 | 107 | Fine |
| `risc/riscv64-asm.c` | 91 | 0 | 63 | 0 | Deliberately minimal; correct as-is |
| `libtcc.h` | 66 | 24 | 24 | 24 | **Public library API — entirely unused** |
| `wasm/wasm-link.c` | 54 | 0 | 0 | 32 | Fine |

Comment density across the tree is 11% (3,156 comment lines / 25,707 code
lines). That is *lower* than the kernel's, and the comments that exist are
mostly upstream TCC's. Not a target.

---

## Complexity ranking — where the difficulty actually lives

698 functions. **Median cyclomatic complexity: 4.** The distribution is
healthy; the problem is entirely in the tail. 33 functions have CC ≥ 50; the
top 22 functions hold 6,863 lines — **26% of all function-body lines in the
tree**.

| CC | lines | args | Function | File |
|---:|---:|---:|---|---|
| 197 | 820 | 0 | `unary` | `tccgen.c:5037` |
| 194 | 408 | 0 | `next_nomacro1` | `tccpp.c:2512` |
| 169 | 608 | 3 | `wasm_emit_function_body` | `wasm/tccwasm.c:1099` |
| 162 | 393 | 2 | `asm_opcode` | `i386/i386-asm.c:498` |
| 128 | 150 | 1 | `gen_opic` | `tccgen.c:2229` |
| 121 | 553 | **19** | `wasm_emit_case` | `wasm/tccwasm.c:546` |
| 118 | 435 | 2 | `asm_parse_directive` | `tccasm.c:490` |
| 109 | 291 | 1 | `parse_number` | `tccpp.c:2221` |
| 103 | 299 | 1 | `preprocess` | `tccpp.c:1688` |
| 103 | 260 | 4 | `tcc_parse_args` | `libtcc.c:1486` |
| 101 | 246 | 5 | `asm_compute_constraints` | `i386/i386-asm.c:975` |
| 96 | 226 | 1 | `gen_cast` | `tccgen.c:3043` |
| 93 | 239 | 2 | `parse_btype` | `tccgen.c:4382` |
| 88 | 331 | 1 | `block` | `tccgen.c:6466` |

**Read this table as the real work list.** Line count is a poor proxy for
complexity; these functions are where a reader actually gets lost.

---

## Verified removable — proof included

### A. Preprocessor-dead blocks: 696 lines, proven semantics-free

Macros never defined by any of the three builds: `CONFIG_TCC_BCHECK` (the
bounds checker, 427 guarded lines), plus `TAL_DEBUG`, `TAL_INFO`, `INC_DEBUG`,
`ASM_DEBUG`, `BF_DEBUG`, `DEBUG_RELOC`, `DEBUG_VERSION`.

Stripped with `unifdef` and rebuilt:

| Target | Result |
|---|---|
| i386 | **byte-identical binary** (`5f820659…`) |
| wasm32 | **byte-identical binary** (`9478d0c8…`) |
| riscv64 | `.rodata`/`.data` byte-identical; `.text` differs in 67 bytes |

The riscv64 `.text` delta is fully explained and inert: `risc/riscv64-gen.c`
contains **28 `assert()` calls**, and `assert` embeds `__LINE__`. Deleting five
lines above them shifts every subsequent assert's encoded line number. The
decisive check: rebuilding the whole riscv64 kernel with the stripped compiler
produces a **byte-identical `kernel.elf`** (`565d17ea597b82f2309d956dd3f2d64f`
both before and after). The compiler emits identical code.

**This is a zero-risk cut and should be taken first.**

Caveat worth recording: a broader `unifdef` run (adding `TCC_TARGET_X86_64`,
`_WIN32`, `__APPLE__`, `CONFIG_TCC_STATIC` and friends — 1,958 lines) **does
not compile**. Those macros appear in `#ifndef`/`#else` positions where
deletion is not a simple excision. The 696-line set is the verified-safe
subset; do not widen it without re-running the same proof.

### B. The `libtcc` public API: 101 lines, linker-proven unreachable

`--gc-sections` discards these in all three targets: `tcc_add_symbol`,
`tcc_get_symbol`, `tcc_list_symbols`, `list_elf_symbols`, `tcc_compile_string`,
`tcc_undefine_symbol`, `tcc_set_lib_path`, `tcc_set_error_func`,
`tcc_get_error_func`, `tcc_get_error_opaque`, `tcc_exit_state`.

Nothing outside `compiler/` includes `libtcc.h` or calls any of it — verified
by grep across the whole repo. TCC-as-a-library is an upstream use case this
project does not have; it only ever runs the `tcc` binary. Removing this also
lets `libtcc.h` shrink to the handful of constants `tcc.h` actually needs.

### C. Stabs debug output: ~520 lines, reachable but never exercised

`stab.def` (234) + `stab.h` (17) + ~270 lines of `put_stabs*` /
`tcc_debug_*` functions, of which `tcc_get_debug_info` alone is 119 lines.
29 of `stab.def`'s 42 stab codes are never referenced anywhere.

This is gated on `do_debug`, set only by `-g`. **Nothing in this project ever
passes `-g`** — not `kernel/Makefile`, not any `demo/` build script, not
`compiler/Makefile`. Stabs is a 1980s debug format that no debugger in this
project's toolchain consumes, and the project's actual debugging method is
serial `kprintf` and QEMU.

Lower confidence than A and B because it is *reachable* code — removing it
means removing the `-g` option, which is a (small) capability loss rather
than a pure deletion. Recommended, but flag it as a deliberate scope decision
rather than dead-code removal.

### D. Dynamic-linking machinery: ~450 lines, needs care

`tcc_load_dll` (143), `store_version` (78), `version_add` (84),
`bind_exe_dynsyms` (86), `bind_libs_dynsyms` (26), `export_global_syms` (21),
plus `.dynsym`/`.dynstr`/`.gnu.version*` setup and `DT_NEEDED`/`.interp`
handling.

Evidence it is unreachable: both musl builds pass `--disable-shared`; every
link in the project is `-static` (`kernel/Makefile`, all four `demo/` build
scripts); no `.so` is produced or consumed anywhere in the tree.

**Important counter-evidence — do not over-cut here.** A *static* link on both
architectures still produces a `.got`, and the i386 static BusyBox still has a
(one-entry) `.plt`. So `put_got_entry`, `build_got_entries`, `fill_got*` and
`create_plt_entry` are **live**, despite reading like dynamic-linking code.
The removable part is dynamic *symbol export and versioning*, not the GOT/PLT
machinery underneath it. Scope accordingly, and prove it the same way A was
proven: byte-identical `kernel.elf` plus a full musl+BusyBox rebuild.

---

## Algorithmic findings, per file

This is the section the question actually asks for. Findings are ranked by
whether they are real.

### `i386/i386-asm.c` — a genuine algorithmic wart

`asm_opcode()` (CC 162, 393 lines) matches every assembled instruction by
**linear scan over the entire 403-entry opcode table**, and can `goto again`
to rescan. The code comment says:

> `/* optimize matching by using a lookup table (no hashing is needed !) */`

…immediately above a loop that is neither a lookup table nor hashed. Sorting
the table by mnemonic token and bucketing by first token — the table is static
and could be indexed at build time — turns this into O(1) plus a short
same-mnemonic walk, and removes the `goto again` retry.

**Honest caveat: this is not a measured bottleneck.** The project assembles
roughly 250 instruction lines of kernel `.S` plus musl-i386's 16 `.s` files.
403 × a few hundred is nothing. Fix it for clarity and because it removes a
retry loop from a CC-162 function, *not* for speed.

### `tccelf.c` — one real O(n·m), one already-correct structure

`tcc_load_alacarte()` dedupes already-pulled archive members with a **linear
scan over a growing array**, inside a loop over the archive index, inside a
`do/while` that repeats until no new members bind. Real numbers from a real
self-hosting link: musl's `libc.a` has **1,273 members and ~1,930 index
symbols**, and the link pulls **196 members**. Worst case is ~378k comparisons
per pass, times the pass count.

A sorted array with binary search, or a small open-addressed set keyed on the
member's file offset, makes the dedup O(1). The existing code carries a long
and genuinely valuable comment explaining *why* the dedup exists (it fixed a
real OOM caused by repeatedly reloading weak-symbol members) — **keep that
comment verbatim**; only the data structure should change.

Again: not a measured bottleneck. Worth doing because "linear scan inside two
nested loops" is exactly the shape that stops being fine when someone links
something bigger.

Conversely, `rebuild_hash()` is **already correct** — the bucket count doubles
(`rebuild_hash(s, 2 * nbuckets)`), giving amortized O(1) insertion. No change.

### `wasm/tccwasm.c` — the worst structural complexity in the tree

`wasm_emit_case()` takes **19 parameters**:

```c
static void wasm_emit_case(WasmBuf *b, WasmFuncIR *f, WasmOp *op,
                           int case_index, int loop_depth, int cur_block,
                           int local_pc, int local_fp, int local_cmp, int local_carry,
                           int local_i0, int local_f0, int local_tmp64,
                           int *op_to_block, int nb_blocks, int emit_dispatch,
                           int stack_reg, int next_first_input, int *p_stack_out);
```

Thirteen of those are loop-invariant for the whole function body. Collapsing
them into a `WasmEmitCtx *` reduces the signature to three or four arguments,
makes every call site readable, and — the real win — makes it possible to
split the 553-line body without threading 19 arguments through each piece.
Paired with `wasm_emit_function_body` (608 lines, CC 169), these two functions
are **1,161 lines, 38% of the entire wasm32 backend**.

This is the single highest-value structural change in the compiler. It is pure
refactoring: no behaviour change, no C-compatibility risk, and the wasm32
target has a fast, self-contained test (`make ARCH=riscv64 test-wasm`) to
prove it.

### `tccgen.c` — large by necessity, with one clean extraction

`unary()` is 820 lines at CC 197. But it is a **dispatch over C's entire unary
expression grammar** — 60+ `case` labels covering every literal type, every
prefix operator, `sizeof`/`_Alignof`, `_Generic`, compound literals, and the
`__builtin_*` family. Most of it is irreducible: it is what "standard C
compatibility" costs, and splitting it arbitrarily would just relocate the
switch.

There is, however, one clean extraction. `unary()` contains ~130 lines of
`__builtin_riscv_*` intrinsics — `syscall`, `csrr`, `csrw`, `sfence_vma`,
`wfi`, `sret`, `ebreak`, `read_fp`, `read_tp`, `write_tp`, `fence_i` — sitting
in the shared front end behind `#ifdef TCC_TARGET_RISCV64`. These exist because
riscv64 deliberately has no real instruction assembler (see
`docs/riscv-port-findings.md`); they are the substitute for inline asm. That
is a sound design decision, but their *location* is not: architecture-specific
code has leaked into the architecture-neutral parser. Moving them behind a
backend hook (`gen_target_builtin(int tok)`, defaulting to "not mine") would
cut `unary()` by ~16%, drop its CC materially, and make the riscv64/i386
seam in the front end explicit rather than incidental.

This mirrors exactly the arch-seam discipline `kernel/` already applies
(`docs/kernel-arch-split-plan.md`) — the compiler simply never had it applied.

`gen_opic()` (CC 128 in only 150 lines) is the densest function in the tree
by ratio: constant-folding for every integer operator, with special cases per
operator and per operand type. It is dense because the problem is dense.
Leave it.

### `tccasm.c` — mild, table-shaped

`asm_parse_directive()` is CC 118 across 435 lines, dispatching 37 directives
in one switch. The project's real `.S` and `.s` files (kernel, musl, BusyBox)
use only **19** of them: `.long .globl .section .align .skip .rept .endr .byte
.word .quad .type .size .set .weak .text .data .bss .ascii .string`.

Converting the switch to a dispatch table of `{token, handler}` would drop CC
sharply and make the supported set legible at a glance. Pruning the unused 18
directives is *possible* but weakly motivated — they are cheap, and unlike C
language features, an assembler directive appearing in some future `.S` file
is a plausible event. Recommend the table, not the pruning.

### `libtcc.c` — table-shaped, same treatment

`tcc_parse_args()` is CC 103 over 260 lines across 50 options. The option
*table* already exists (`{ "static", TCC_OPTION_static, 0 }`, …); the giant
switch that consumes it is what carries the complexity. Several arms are dead
in this project's usage: `-shared`, `-soname`, `-rdynamic`, `-bench`, `-b`
(bounds check — the feature is already `#ifdef`'d out, see §A), and `-g` if §C
is taken. Note that `-r` (partial link) **is** used — BusyBox's `trylink`
depends on it — so do not cut it.

### `tccpp.c` — algorithmically sound, leave alone

`next_nomacro1()` is the hottest function in the tree (560,614 calls in a real
self-build) and the second most complex (CC 194, 408 lines). It is a hand-rolled
character-dispatch lexer, and its complexity is inherent to that job. The
profile shows it costs 0.01s. **Do not touch it.** Any restructuring risks
lexer correctness — the highest-blast-radius area in the whole compiler — for
no measurable gain.

The token table uses `table_ident[]` indexed directly by token id, with
`sym_identifier`/`sym_struct` pointers hanging off each entry. That is already
O(1) symbol lookup with O(1) scope shadowing. Nothing to improve.

### `tcc.h` — a coupling problem, not a size problem

Every one of the eleven `.c` files includes exactly one header: `tcc.h`. It is
1,314 non-blank lines and transitively pulls in `elf.h`, `stab.h`, `tcctok.h`,
`libtcc.h` and `config.h`. It declares the *entire* internal API — parser,
preprocessor, ELF writer, assembler and backend — with no separation.

Consequences: any header edit rebuilds everything, and there is no enforced
boundary between subsystems (nothing stops `tccpp.c` reaching into ELF state).
Splitting into `tcc-pp.h` / `tcc-gen.h` / `tcc-elf.h` / `tcc-backend.h` with a
small shared `tcc-common.h` would add a few lines net while making the
subsystem boundaries real. This is the compiler's equivalent of the
`syscall_common.h` / `sched/process.h` seams the kernel already has.

Worth noting the measurement caveat that led here: `tcc.h` shows only ~47%
"used" in the preprocessed-line table, but that number is an artifact — its
`#define`s are consumed by the preprocessor and never appear in output. By
symbol reference, only **21 of its 348 macros** are unreferenced. `tcc.h` is
not bloated; it is undivided.

### Headers assessed by symbol reference

| Header | Macros defined | Never referenced elsewhere |
|---|---:|---:|
| `elf.h` | 177 | 38 (21%) |
| `tcc.h` | 348 | 21 (6%) |
| `tcctok.h` | 256 | 10 (3%) |
| `stab.def` | 42 | 29 (69%) |
| `libtcc.h` | 6 | 1 |

`elf.h`'s remaining 38 unreferenced macros are the tail left by the §2 prune
(3,290 → 1,162 lines) and are worth a second pass, but the yield is small.

**`i386/i386-asm.h` and `i386/i386-tok.h` are deliberately excluded from this
table.** A naive scan reports 88% and 50% "unreferenced", which is a **false
positive**: they are X-macro tables (`DEF_ASM_OP0(clc, 0xf8)`, 403 entries)
whose names are consumed by repeated inclusion under different macro
definitions, not by direct reference. They are *data*, and 403 rows of opcode
data is not complexity in any sense that matters. Do not cut them on a
line-count argument.

For the record, the project's real i386 assembly (kernel `.S` + musl-i386's
16 `.s` files + inline asm) uses **47 distinct mnemonics** of the 403 encoded.
That gap is insurance, not waste.

---

## Why the small-compiler comparison doesn't transfer

The premise — "there are lots of compilers smaller than 35k lines" — is true,
and worth taking seriously. `chibicc` is ~9k lines and implements most of C11;
`cproc` and `lacc` are each ~15k. (**These three figures are from general
knowledge, not measured — they are the only unmeasured numbers in this
document.** The comparison below does not depend on their precision.) So what
do they have that this doesn't?

**They emit assembly text and shell out to `as` and `ld`.**

That is the entire difference, and it is not available here. This project's
defining constraint is that TCC must run *inside its own kernel*, where there
is no `as`, no `ld`, no shell to invoke them, and no second toolchain. The
compiler must go from `.c` to a linked ELF executable in one process. Removing
`tccasm.c` + `i386/i386-asm.*` + `tccelf.c` + the `*-link.c` files — the 8,308
lines that exist only because there is no external toolchain — would produce a
compiler in the 25k range that **cannot achieve the project's central goal.**

Adjust the comparison accordingly:

| | Lines | Emits | Links |
|---|---:|---|---|
| chibicc | ~9k | assembly text | external `ld` |
| cproc | ~15k | QBE IR | external `as`+`ld` |
| **this tree, per target** | **17–18k** | **machine code** | **itself** |
| upstream TCC (all targets) | ~60k | machine code | itself |

Against self-contained compilers that also assemble and link, 17–18k per
target is already at the small end. The honest statement is not "the compiler
is 33k lines and should be 10k" — it is "**each build is 17–18k lines and does
strictly more than the 9k compilers do**".

---

## What not to do

- **Do not restructure `next_nomacro1` or `parse_number`.** Highest complexity,
  highest blast radius, zero measured cost.
- **Do not prune the i386 opcode tables** on a line-count argument. They are
  data; the 47-of-403 usage figure is insurance against the next `.S` file, and
  inline asm is exactly where a missing entry becomes a confusing build failure
  in someone else's project (musl, BusyBox).
- **Do not remove GOT/PLT code as part of a "we only link static" cleanup.**
  Static links on both architectures demonstrably produce `.got` (and `.plt` on
  i386). Verified above.
- **Do not drop GNU extensions** in pursuit of "standard C only". `__attribute__`
  is load-bearing for BusyBox (its `platform.h` shim is why the GCC-version
  spoof exists at all) and inline `asm` is load-bearing for musl. Standard C
  compatibility is a floor, not a ceiling.
- **Do not delete the long explanatory comments** in `tcc_load_alacarte` or the
  riscv64 intrinsics. They encode real debugging history, exactly as
  `docs/*-findings.md` do.

---

## Recommended order

Ranked by (value ÷ risk), not by line count.

| # | Item | Lines | Risk | Why this order |
|---|---|---:|---|---|
| 1 | §A preprocessor-dead strip | −696 | **none** | Already proven byte-identical; take it immediately |
| 2 | §B `libtcc` public API | −101 | none | Linker-proven dead |
| 3 | `wasm_emit_case` context struct | ~0 | low | Biggest *complexity* win in the tree; isolated target with its own test |
| 4 | riscv64 intrinsics → backend hook | ~0 | low | Fixes a real arch-seam leak; −16% on the most complex function |
| 5 | §C stabs removal | −520 | low | Capability decision, not dead code — decide deliberately |
| 6 | `asm_opcode` table dispatch | ~−40 | low | Removes a retry loop from a CC-162 function |
| 7 | `tcc_load_alacarte` dedup set | ~0 | low | Removes the one real O(n·m) in the tree |
| 8 | `asm_parse_directive` / `tcc_parse_args` dispatch tables | ~0 | low | Large CC reduction, no size change |
| 9 | §D dynamic-symbol export/versioning | ~−450 | **medium** | Must not touch GOT/PLT; prove like §A |
| 10 | `tcc.h` split | ~+30 | medium | Best long-term structure, worst churn-to-line ratio |

**Total line reduction available at low-or-no risk: ~1,320 (≈4%).**
**Total complexity reduction available: substantial** — items 3, 4, 6, 8 between
them address five of the twelve most complex functions in the tree without
removing a single feature.

That asymmetry is the real conclusion of this review. `compiler/` is not
carrying much dead weight; it is carrying a small number of badly-shaped
functions and two backends this build isn't using. Optimising for line count
here has largely run out of road — §2 and §3 of the previous review already
took the 8,259 lines that were genuinely free. What is left to improve is
shape, not size.
