# Whole-repo review — 2026-08-26

Scope: every tracked file (60,090 lines), reviewed after the arch split
(`86c1879`), the compiler's separate-compilation restructure (`976f559`), and
i386 self-hosting closure (`b31f8e5`). It supersedes the pre-split parts of
`complexity-review-2026-08-25.html` and `kernel-complexity-review.md`.

Every claim below was measured or run, not read off the source. Where a
finding is a *proposal*, it says so; where it was proven, the proof is stated.
Two things were verified so thoroughly they were acted on during the review
itself, and are called out as already-done.

**Health check first.** All eleven test targets were run tonight and pass:
i386 `test`/`test-initrd`/`test-busybox`/`test-selfhost`, riscv64
`test`/`test-initrd`/`test-selfhost`/`test-wasm`/`shell-wasm`/
`test-initrd-wasm`, and the three compiler target builds. The repo does what
it claims. Everything below is about *how much machinery* it takes to do it,
plus one correctness problem found along the way.

**Update, 2026-08-27: sections 2, 3, 4, and half of 6 are done.** Each is
marked `DONE` inline with its commit. Net: **8,756 lines removed** across
three commits (`b9cbabd`, `1f9adbe`, `cb1ee23`), with no loss of function —
every test target above was re-run after each change and still passes. §1
(the i386 fragility) was deliberately *not* attempted as part of this work —
see its own section for why proceeding with §2/§4 anyway, and what came of
it, given the review's own original advice was to root-cause §1 first.

---

## Where the lines are

As measured 2026-08-26, before any of the work below. See the update note
above for current totals (51,715 tracked lines overall, post-simplification).

| Area | Lines | Note |
|---|---:|---|
| `compiler/` | 37,806 | 63% of the repo |
| `kernel/` | 13,373 | of which 3,696 is one generated payload header |
| `demo/` | 4,293 | 3,846 of it vendor patches |
| `docs/` | 3,475 | 18 files |
| `emulator/` | 953 | tight; no findings |
| `cpu/` | 21 | placeholder README, correctly scoped |

---

## 1. Correctness: i386 in-kernel self-hosting currently passes by luck

**This is the most important finding in the review and the only one that is a
risk rather than an opportunity.** It is a latent i386 kernel bug, not a
compiler bug.

While testing an unrelated simplification (§2), `make ARCH=i386 test-selfhost`
began failing deterministically with:

```
SELFHOST: stage1 compiling TCC
tcc: error: undefined symbol '__vdsosym'
```

The evidence rules out every ordinary explanation:

- **Not a compiler change.** The change that triggered it (deleting unused
  macros from `compiler/elf.h`) produces a **byte-identical** `stage1/tcc` —
  same md5 (`df816815d2d4ce5322d3a4cb50cfc4ec`) before and after. The failing
  and passing runs use the *same compiler binary*.
- **Not a compiler bug.** That identical stage1 binary performs the identical
  stage2 link **correctly** under `qemu-i386-static` on the host.
- **Not a truncated or missing input.** `ls -l /musl/lib/libc.a` inside the
  kernel reports 1,909,214 bytes — exactly the host size.
- **Not archive-resolution logic.** With `-vv`, the in-kernel link pulls
  **194 archive members, in the same order, and the sets are identical** to
  the host run — `vdso.o` included, at position 182 in both. The member that
  defines `__vdsosym` *is* loaded, and the symbol still fails to resolve.
- **Not memory exhaustion.** `pmm` reports 129,636 KB free, and the test
  passes at `-m 80`, `96`, `112`, `120`, and `128`.
- **Not riscv64.** The identical perturbation leaves `ARCH=riscv64
  test-selfhost` passing.

And the decisive one: **adding two `kprintf()` diagnostics to the kernel made
the failing configuration pass again.** The only variable that actually moves
is the in-kernel memory/allocation layout of the process doing the link.

That is the signature of a layout-sensitive latent bug in the i386 kernel
path — memory that is written or read slightly out of bounds, or state that
survives where it shouldn't, in code all of which was written within the last
day. The headline claim "TCC compiles TCC inside our kernel" holds for i386
today, but it holds *by luck of layout*, and any future change to the kernel,
the initrd contents, or the compiler can flip it without warning.

**Recommended before any further i386 work.** Root-cause it rather than
re-tuning around it. The suspects worth eliminating first, in order:

1. `struct regs` gained `useresp`/`ss` for checkpoint 17. Those two words are
   only pushed by the CPU on a *ring-3→ring-0* trap. `process_arch_save_trapframe()`
   copies `sizeof(struct regs)` words from `idt_current_trapframe()`
   unconditionally; any path that reaches it while the live frame is a
   ring-0 trap reads two words past the frame and stores them as user state.
   Audit whether the PIT (still unmasked on i386 — riscv64 deliberately calls
   `timer_disable()`) can ever make that true.
2. The single global `idt_current_trapframe` pointer: confirm no trap can
   overwrite it between a syscall's entry and a `process_schedule()` inside
   that syscall.
3. `paging_new_addrspace()` shares whole 4 MB PDEs up to `kernel_pd_top`. With
   `-m 128` that boundary is 0x08000000 and the first user ELF loads at
   0x08048198 — clear by 294 KB. That is a very thin margin for a value
   derived from a QEMU flag; assert it rather than rely on it.

A cheap regression harness for this class of bug: run `test-selfhost` against
two or three deliberately different initrd paddings. If the result depends on
padding, the bug is still there. **That harness has still never actually been
run** (see below) — it remains the fastest way to get a real signal instead of
more anecdotal passes.

**Status, 2026-08-27: still unresolved, not fixed.** §2 and §4 below were both
landed anyway (user decision, against this section's own original advice to
root-cause this first) — both are exactly the kind of memory-layout-shifting
change that flipped the bug the first time (§2 is literally the change that
found it; §4 cut `kmain.c` by ~250 lines, replaced the embedded ELF payload
with a real initrd file, and shrank `kernel.elf` by half). `test-selfhost` was
re-run **three times back to back** after landing §4 and passed every time.
That is evidence, not proof — the bug's whole signature is "passes reliably
until something shifts memory, then fails deterministically until something
shifts it back". If `test-selfhost` (i386) ever fails again, check this
history before assuming it's a new regression, and finally run the padding
harness above rather than adding more anecdotal passes to this tally.

## 2. `compiler/elf.h`: 2,128 of 3,290 lines are dead — proven zero-risk

**DONE (`b9cbabd`).**

`elf.h` defines 2,219 macros. Cross-referencing every compiler source
(including `ElfW()`/`ELFW()` token-paste expansions) shows **153 are used and
2,066 are never referenced** — 93%. They are relocation and machine constants
for architectures this compiler cannot target: 124 `R_AARCH64_*`, 121
`R_TILEGX_*`, 110 `R_PPC64_*`, 110 `R_PARISC_*`, 95 `R_SPARC_*`, 93 `R_PPC_*`,
92 `R_TILEPRO_*`, 81 `R_IA64_*`, 67 `R_ARM_*`, 63 `R_390_*`, and so on — 1,330
dead relocation constants in total, plus 79 `EM_*`, 108 `DT_*`, 96 `EF_*`.

Pruning them (transitive closure, so anything referenced from a kept
definition stays) leaves **1,162 lines, saving 2,128**. Verified:

- all three targets build (`i386`, `riscv64`, `wasm32`);
- the resulting `stage1/tcc` is **byte-identical** to the unpruned one, which
  is the strongest possible evidence that nothing semantic was removed;
- riscv64 `test`, `test-initrd` and `test-selfhost` all pass against it.

This was the single largest genuinely free deletion in the repo. It's also
the change that exposed §1's i386 fragility in the first place — the
original recommendation here was to root-cause that first and land this
after; it was landed anyway (user decision) and has passed every re-run
since (§1's own updated status).

`kernel/mm/elf.h` is a separate, already-minimal 70-line header and is not
affected — nothing outside `compiler/tcc.h` includes `compiler/elf.h`.

## 3. `compiler/`: 2,563 lines of orphaned upstream files

**DONE (`b9cbabd`, same commit as §2).** All ten removed, each re-verified
unreferenced immediately before deletion (a basename grep can have false
positives — e.g. `TODO`/`VERSION` as plain words in comments and macros — each
hit was checked individually). `COPYING`/`RELICENSING` kept, as recommended.

Files that were reachable from no build input, no script, and no other
tracked file:

| File | Lines | |
|---|---:|---|
| `tcc-doc.texi` | 1,302 | upstream manual; referenced only by `Changelog` |
| `Changelog` | 439 | upstream history to 2017 |
| `texi2pod.pl` | 427 | toolchain for the manual above |
| `TODO` | 97 | upstream wishlist |
| `tcclib.h` | 80 | referenced only by the upstream `README` |
| `CodingStyle` | 71 | |
| `README` | 71 | upstream TCC readme, superseded by our `README.md` |
| `lib/va_list.c` | 59 | not in `LIBTCC1_OBJS` for any target |
| `risc/fetch_and_add_riscv64.S` | 16 | not in any target's `RUNTIME_SRCS` |
| `VERSION` | 1 | dead: `config.h` hardcodes `TCC_VERSION` |

**Keep `COPYING` (504) and `RELICENSING` (60)** — TCC is LGPL and both are
licensing records, not documentation.

## 4. `kernel/`: i386 never got the product/checkpoint split riscv64 has

**DONE (`1f9adbe`).** `arch/i386/kmain.c` now mirrors `riscv64_kmain.c`'s own
`#ifdef KERNEL_CHECKPOINTS` shape exactly: shared hardware/memory/filesystem
bring-up, then either `run_checkpoint_boot()` (the historical chain, now in a
new `test/i386_checkpoints.c` — a straight move, not a rewrite) or straight to
an interactive BusyBox shell. The one genuine change, not just a move: P5
checkpoint 2's real musl+TCC binary is now `user_test/hello_i386.c`, built and
loaded from the checkpoint chain's own small initrd the same way
`test/riscv64_checkpoints.c` already loads `hello_riscv64.c` — not a
pre-built hex dump with no live rebuild path. `arch/i386/hello_elf_payload.h`
is gone outright; `task.c`/`pit.c`/`usermode.S` moved out of the product
`OBJS` into a new `I386_CHECKPOINT_OBJS`, mirroring `RISCV64_CHECKPOINT_OBJS`.
Measured result: **`kernel.elf` shrank from 94,596 to 45,444 bytes (52%)**.
Full sweep (both `kernel.elf` and the new `kernel-checkpoints.elf`, both
architectures) passes — see §1 for the one open question this raised.

Commit `c605aa3` moved riscv64's P1–P10 chain into
`test/riscv64_checkpoints.c`, built only under `-DKERNEL_CHECKPOINTS`, so the
product kernel contains none of it. **i386 still links its entire historical
checkpoint chain into the product kernel on every build.**

The cost is concrete and large:

- `arch/i386/hello_elf_payload.h` is **3,696 lines** — 28% of the whole
  kernel — and is a hex dump of a 44,228-byte musl+TCC test binary. That
  payload is **47% of the i386 `kernel.elf` image** (44,228 of 94,596 bytes).
  The riscv64 kernel is 62,020 bytes and embeds nothing.
- It contradicts the project's own stated rule, written in three places:
  "The kernel embeds no file contents" (`mm/ramfs.h`), "contains no ramdisk
  bytes" (`arch/risc/riscv64_kmain.c`), "The kernel contains no file payload
  bytes" (`kernel/Makefile`). That rule is true of riscv64 and false of i386.
- Checkpoint-only i386 files total **3,854 lines**: the payload header (3,696),
  `user_test_payload.h` (18), `gen_user_test_header.py` (40), `task.c` (53,
  the superseded fixed-2-task scheduler), `pit.c` (19), `usermode.S` (28).
  `enter_usermode`, `task_*` and `pit_init` have no caller outside
  `kmain.c`'s checkpoint functions — the product path uses the process layer
  and `i386_trap_return` instead.
- A further **140 of `kmain.c`'s 418 lines** are checkpoint scaffolding
  (measured by attributing each function), against 126 lines of product boot.

This was the largest structural simplification in the review: it removed
~29% of the kernel's source and 47% of the i386 image, and it makes the two
architectures describable by one sentence instead of two.

## 5. `kernel/`: duplication introduced by the arch split

Small individually, but each is a "fix it in N places" hazard, and some of it
is a day old:

- **Path canonicaliser, three copies** — `syscall_posix.c`, `arch/i386/syscall.c`
  (`resolve_i386_path`), `arch/risc/riscv64_syscall.c` (`resolve_path`). The
  logic is completely architecture-neutral; it is duplicated only because the
  shared copy is `static`. Export it once (~50 lines saved, and one place to
  fix a path bug instead of three). Worth noting that a *missing* cwd
  resolution in exactly this area was a real i386 bug fixed in `4a4dfd0`.
- **`stat` lookup, two copies** — the `is_dir → dynamic → fixed` lookup order
  is identical in both arches' `syscall.c`; only `fill_stat`'s byte offsets
  genuinely differ (i386 96-byte `kstat`, riscv64 128-byte). Share the lookup,
  keep the layout per-arch (~20 lines).
- **`struct fd_entry` copied by hand in five places** — `process_fork()` twice,
  `process_fd_set()`, `process_stdio_set()`, `sys_fcntl()`. Each is ~8 field
  assignments plus a 128-byte path loop. One `fd_entry_copy()` removes ~40
  lines and, more importantly, the risk that a sixth field gets added to the
  struct and missed in one of the five.
- **`copy_regs()`, two copies** differing only in word width.

## 6. `demo/`: whole-file-deletion patch hunks, and the two BusyBox patches

**First half DONE (`cb1ee23`).** Checked all four patches for hunks that
delete a file outright (`deleted file mode`, cross-checked against the
alternate `+++ /dev/null` marker some diff tools use instead — both give the
same answer). Only `musl-riscv64-tcc-compat.patch` had any: **22**, roughly
half its own line count. Each was verified against a fresh upstream musl
v1.2.4 clone before being extracted, to tell whole-directory deletions
(`src/fenv/riscv64/`, `src/math/riscv64/`, `src/ldso/riscv64/` — every file
those leaf directories contain is gone, safe as `rm -rf`) from directories
that keep other, differently-modified files (`crt/`, `src/setjmp/riscv64/`,
`src/signal/riscv64/`, `src/thread/riscv64/` — 7 individual `rm`s). Verified
by running the real build end to end against the trimmed patch: musl-riscv64
built successfully, its own smoke test passed, and — the meaningful proof,
since `fork()`/`setjmp`/`longjmp` are exactly what the deleted files touched
— a full `make ARCH=riscv64 clean && test && test-initrd && test-selfhost`
against the freshly-rebuilt `libc.a` all passed. Patch: 1,107 → 568 lines
(49%). The other three patches (`busybox-i386`, `busybox-riscv64`,
`musl-i386`) have zero whole-file deletions — nothing to extract there.

**Second half still open.** `patches/busybox-i386-tcc-compat.patch` (1,373
lines) and `patches/busybox-riscv64-tcc-compat.patch` (1,295) differ in only
**121 lines after normalising the arch name** — and a large part of that
difference is a `build.sh` embedded inside the i386 patch that duplicates the
real `demo/build-busybox-i386.sh`, plus index hashes and timestamps.

Deleting the embedded `build.sh` hunk is unambiguous. Unifying the rest into
one patch plus a small arch delta would remove ~1,200 duplicated lines, but
patches are brittle and the payoff is maintenance-only; worth doing only if
these are expected to change again.

The **build scripts themselves are not duplicated** — `build-musl-i386.sh` vs
`build-musl-riscv64.sh` differ on 111 of 116 lines, and the BusyBox pair on
146 of 165. They diverged for real reasons. Leave them alone.

## 7. `docs/`: drift and overlap

- **Stale as of today.** `README.md` describes i386 as "a second
  implementation and self-hosting check" and documents only
  `ARCH=riscv64 test-selfhost`; `self-hosting-todo.md` is titled "In-kernel
  self-hosting: complete" but describes riscv64 exclusively. i386 reached the
  same closure bar in `b31f8e5` (`make -C kernel ARCH=i386 test-selfhost`).
  Both should be updated — this is my own omission from today's work.
- **Four overlapping review documents** now exist:
  `complexity-review-2026-08-25.html`, `compiler-unused-options-review.md`,
  `compiler-simplification-opportunities.md`, `kernel-complexity-review.md`,
  plus this one. The first is pre-arch-split and largely superseded. Suggest
  keeping one current review and moving the rest under a `docs/history/`
  prefix, so a reader can tell which describes the code as it is.
- The seven `*-findings.md` files are point-in-time records of real debugging
  and are worth keeping as-is; they are cited by name from source comments.
- `compiler/Makefile:108` still says the "riscv64 kernel port not yet
  started".

## 8. Minor

- `.gitignore` does not cover `kernel/initrd-i386.tar` or
  `kernel/initrd-i386-bb.tar` — the existing pattern is `kernel/*-initrd.tar`,
  which does not match `initrd-i386.tar`. Both appear as untracked after any
  i386 test run. (My omission, from `ab87337`.)
- `.gitignore` carries four entries already covered by `*.elf`.
- `kernel/Makefile` is 533 lines, 227 of them comments, with two arch branches
  whose targets differ in name for the same job (`test-busybox` vs
  `shell-wasm`, `selfhost-initrd-i386` vs `selfhost-initrd`). Parameterising
  QEMU binary/flags/initrd-mechanism would fold most of the two branches
  together and remove the naming asymmetry.
- `wasm32` builds one genuinely unused function (`elf_output_file`); both other
  targets build clean under `-Wunused-function`. The compiler is otherwise
  free of dead code — the earlier removal passes did their job, and there are
  no remaining references to unsupported targets outside `elf.h`.

---

## Already done

**Out-of-bounds read past EOF — found, proven, fixed, committed (`6fd0e2c`).**

`read_from_fd_entry()` computed `src_size - entry->pos` in unsigned arithmetic
with no check that `pos` was still inside the file. `sys_lseek()` deliberately
allows seeking past EOF (POSIX-legal, and what `tccelf.c`'s sparse ELF writer
relies on), so `pos > size` is reachable — and there the subtraction underflows,
the copy loop runs for the caller's full `count`, and the syscall returns bytes
read off the end of the file's backing store: kernel memory, handed to
userspace as file contents, with a non-zero return where Linux returns 0.

Confirmed with a probe binary rather than left as a reading of the code:
against a 15-byte initrd file, `lseek(fd, 10000, SEEK_SET); read(fd, buf, 8)`
returned **8**, with the buffer holding kernel heap bytes
(`03 64 76 00 03 80 76 00`). After the fix the same probe returns 0 and leaves
the buffer untouched. It is in `syscall_posix.c`, so it affected **both**
architectures. All eight QEMU test targets pass with the fix.

---

## Remaining, in suggested order

1. **Root-cause §1**, now that §2 and §4 have actually exercised its risk
   twice more (and survived, so far) rather than just theorized about it. The
   padding-based regression harness described there has still never been run.
2. **§5, the split's duplication.** Cheap, and it is the category that has
   already produced one real bug.
3. **§6's second half**, the BusyBox patch overlap — lower priority, patches
   are brittle and this is maintenance-only payoff.
4. **§7, docs.** Cheap, and the README currently understates what the project
   does — including, now, that i386 self-hosts too.
5. **§8, minor** — `.gitignore` gaps, `kernel/Makefile`'s naming asymmetry
   between arch branches.

**Done so far: 8,756 lines removed** (`b9cbabd` −4,680, `1f9adbe` −3,579,
`cb1ee23` −497), with no loss of function — the compiler still targets
i386/riscv64/wasm32 and builds musl, BusyBox, both kernels and itself; both
kernels still self-host TCC; all eleven QEMU/wasm test targets still pass.

## What not to simplify

- The `*-findings.md` documents. They are cited from source comments and they
  are why the non-obvious code in this repo is explicable.
- The dense "why this is the way it is" comments in `kernel/` and the backends.
  They are unusually load-bearing here, several of them encoding bugs that
  cost real debugging, and they should survive any of the deletions above.
- `emulator/` (953 lines) and `cpu/` (a scoped placeholder). Nothing to do.
- The per-arch build scripts in `demo/` (§6) — genuinely divergent, not
  duplicated.
- The three compiler backends. Keeping i386 alongside riscv64 is what caught
  the shared-code bug fixed above, and what makes the "second implementation"
  claim real rather than aspirational.
