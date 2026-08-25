# Kernel review: complexity that can be simplified

Read-only review of `kernel/` at commit `a2ba681`, done after the
directory was declared feature-complete. No source was changed to
produce this document.

Context first, because it changes what "simplify" should mean here: the
kernel is now **11,617 tracked lines, down from 75,929** at the last
review. The generated-payload problem both earlier reviews led with is
solved for riscv64 — BusyBox and the process-test fixtures now arrive
through a real tar initrd, and `kernel.elf` is a 62KB binary containing
no file payload bytes at all. The remaining findings are genuinely
about structure, not bulk deletion.

Measured breakdown of what's left:

| Area | Lines | Note |
|---|---:|---|
| riscv64 kernel source | ~4,700 | the actual product |
| i386 kernel source | ~3,105 | second target, structurally separate |
| `hello_elf_payload.h` + `user_test_payload.h` | 3,714 | generated C byte arrays — **i386 only** |
| Build/test harness | ~570 | `Makefile`, `boot_test.py`, initrd scripts |
| riscv64 user-test fixtures | ~280 | small, real programs |

The single biggest structural theme below is that **the boot path and
the test suite are the same code**, spread across three subsystems. That
one issue accounts for most of the cross-module coupling in the tree.

---

## 1. The checkpoint chain is wired through the syscall layer and the scheduler

**Severity: high — this is the tree's main structural coupling.**

Booting to a shell currently runs through this chain, which crosses
module boundaries four times:

```
kmain()                          riscv64_kmain.c
  └─ task_a/task_b (P4 demo)     riscv64_kmain.c
      └─ conclude_scheduler_test
          └─ run_ring3_test
              ↓ sys_exit()       arch/riscv64_syscall.c   ← syscall layer
          └─ run_elf_test                                   calls a test fn
              ↓ sys_exit_group() arch/riscv64_syscall.c   ← again
          └─ run_process_test
              ↓ drain_hook       sched/riscv64_process.c  ← scheduler
          └─ run_fork_test → run_exec_test → run_init_test → run_interactive_test
              ↓ halt_process_test()
                 prints "P10 checkpoint OK"  ← in the scheduler
```

Concrete consequences, all verifiable in the current source:

- `kernel.h` — the kernel-wide interface header, next to `kprintf` and
  `serial_init` — declares two **test functions**:
  ```c
  void run_elf_test(void);     /* riscv64_kmain.c -- see arch/riscv64_syscall.c's sys_exit */
  void run_process_test(void); /* riscv64_kmain.c -- see arch/riscv64_syscall.c's sys_exit_group */
  ```
- `arch/riscv64_syscall.c:235,251` — `sys_exit`/`sys_exit_group` call
  those test functions directly, and print `"P5 checkpoint 1 OK"` /
  `"P5 checkpoint 2 OK"`. A syscall implementation knows which
  checkpoint runs next.
- `sched/riscv64_process.c:314` — `halt_process_test()` prints
  `"P10 checkpoint OK"`. The scheduler's shutdown path hardcodes a
  milestone label, with a comment conceding it will need bumping if a
  checkpoint 11 is ever added.
- `sched/riscv64_process.c:296` — the `drain_hook` mechanism (a global
  function pointer fired when the process table empties) exists purely
  to sequence tests.

`KERNEL_DIRECT_SHELL` already proves the product boot needs none of
this: it's a 4-line `#ifdef` in `kmain` that calls `syscall_init()`,
`process_init()`, `run_interactive_test()` and skips the entire chain.

**Simplification.** Invert the relationship. Make the direct path the
default boot, and move the checkpoint sequence into a test-only driver
compiled in under a flag (the mirror image of today's
`KERNEL_DIRECT_SHELL`). That deletes `drain_hook` from the scheduler,
the two `run_*_test()` declarations from `kernel.h`, the test calls and
`"P5 checkpoint"` strings from the syscall layer, and the `"P10"` string
from `halt_process_test()`. Estimated ~80 lines removed and, more
importantly, three subsystems stop knowing about the test suite.

## 2. The five `run_*_test` functions are one table

`run_fork_test`, `run_exec_test`, `run_init_test`,
`run_interactive_test` (riscv64_kmain.c:276–347) are the same twelve
lines four times, differing only in three strings:

```c
kprintf("P<N> checkpoint OK\n");
process_set_drain_hook(<next>);
struct process *p = process_from_initrd("<file>", "<name>");
if (!p) { kprintf("FATAL: process_create_from_elf failed\n"); for(;;) wfi(); }
kprintf("process: <label> process created (pid %d)\n", p->pid);
process_run(p);
```

A `static const struct { const char *label, *file, *proc_name; }
checkpoints[]` plus one driver loop replaces all four with roughly 15
lines total. **~35 lines removed**, and adding a checkpoint becomes a
table row instead of a new function plus a hook rewire.

## 3. Three separate implementations of "build a user stack"

The argc/argv/envp/auxv stack layout — the exact same AT_PAGESZ-carrying
block, with the same 64/128-byte offset convention — is hand-built in
three places:

| Location | Lines | argv |
|---|---:|---|
| `riscv64_kmain.c:200` `run_elf_test()` | ~50 | fixed `"hello"` |
| `sched/riscv64_process.c:131` `process_create_from_elf()` | ~115 | single `arg0` |
| `sched/riscv64_process.c:678` `process_execve()` | ~130 | real `argv[]` |

The latter two are near-identical beyond argv handling: allocate address
space, `elf_load`, map+zero the initial stack pages, record
`user_stack_lo/hi/limit` + `user_brk` + `user_mmap_next`, write the
pointer block, build the initial trapframe. `process_create_from_elf` is
strictly `process_execve` with a simpler argv and a new process slot
instead of the current one.

**Simplification.** One internal `build_process_image(root, elf, size,
argv[], argc)` returning `{entry, sp}`; creation installs it in a fresh
slot, exec installs it into the current one, and `run_elf_test` either
uses it too or disappears with the checkpoint chain (§1). Estimated
**~120 lines removed** — the largest single mechanical win in the tree,
and both earlier reviews independently recommended it.

## 4. `fd_entry` copying is open-coded five times

`struct fd_entry` has six fields plus a 128-byte path. Copying one is
written out longhand in five places:

- `process_fork()` — the `fds[]` loop (riscv64_process.c:481)
- `process_fork()` — the `stdio_override[]` loop (:501)
- `process_fd_set()` (:604)
- `process_stdio_set()` (:641)
- `sys_fcntl()`'s `F_DUPFD` branch (riscv64_syscall.c:1178)

Each repeats `dst->data = src->data; dst->size = ...;` and a
`for (int i = 0; i < 128; i++) dst->path[i] = src->path[i];` loop — with
the literal `128` rather than `sizeof`. The reason struct assignment is
avoided is real and correctly documented (TCC would emit a `memmove`
call this freestanding kernel never links), but that argues for **one**
`static void fd_entry_copy(struct fd_entry *dst, const struct fd_entry
*src)` helper, not five copies. A field added to `fd_entry` today must
be remembered in all five. **~45 lines removed**, and a real class of
silent bug closed.

## 5. Zero-a-freshly-allocated-page is written out six times

`pmm_alloc_page()` returns non-zeroed memory, so callers zero it
themselves with this identical loop:

```c
unsigned long *words = (unsigned long *)phys;
for (unsigned int i = 0; i < PAGE_SIZE / sizeof(unsigned long); i++)
        words[i] = 0;
```

It appears in `sys_brk`, `sys_mmap`, `sys_mremap` (riscv64_syscall.c),
`process_create_from_elf`, `process_execve`, `process_handle_stack_fault`
(riscv64_process.c), and in a byte-wise variant in `mm/elf.c`'s
`load_segment` and `alloc_table` in `riscv64_paging.c`.

Every current caller wants zeroed memory. A `pmm_alloc_zeroed_page()`
next to `pmm_alloc_page()` in `mm/pmm.c` removes ~30 lines and makes the
one genuinely security-relevant invariant here ("user-visible pages
never expose a previous allocation's contents") a property of the
allocator rather than of six call sites remembering to do it. This is
not hypothetical: a comment in `process_execve` records that forgetting
it was investigated as the cause of a real fault.

## 6. `ramfs_dir_entry()` is O(n²) and sits on the hot path

`mm/ramfs.c:255`. For each raw slot it re-derives every *earlier* slot's
child name to deduplicate:

```c
for (unsigned int raw = 0; raw < raw_count; raw++) {
        if (!raw_child(dir, raw, candidate, ...)) continue;
        for (unsigned int earlier = 0; earlier < raw; earlier++) {
                if (raw_child(dir, earlier, previous, ...) && streq(...)) ...
        }
        if (accepted++ == index) return 1;
}
```

`raw_count` is `RAMFS_MAX_DYNAMIC_FILES + NUM_APPLETS` = **552**, so one
call costs up to ~152,000 `raw_child` invocations (each doing prefix and
string comparison). Two multipliers make that matter:

- `sys_getdents64` calls it **once per returned entry**, restarting the
  scan from zero each time — listing a directory of *N* entries is
  O(N × 552²).
- `ramfs_is_dir()` (ramfs.c:280) is implemented as
  `ramfs_dir_entry(dir, 0, ...)` — the full deduplicating scan just to
  answer "does this path have any child?" — and it is called on **every
  path lookup**: `sys_openat`, `sys_faccessat`, `sys_newfstatat`,
  `sys_chdir`.

The self-hosting build opens hundreds of headers, so this is exercised
hard. Two independent fixes, neither large: give `ramfs_is_dir` an early
`return 1` on the first matching child rather than routing through the
deduplicating enumerator, and have `sys_getdents64` keep a resume cursor
in the `fd_entry` (there is already a `pos` field) instead of re-scanning
from zero per entry.

## 7. `struct ramfs_file` is vestigial, and forces a dual lookup everywhere

All real files are dynamic now. `struct ramfs_file` and `ramfs_lookup()`
survive only to resolve BusyBox applet aliases, and `ramfs_lookup`
returns a pointer to a **function-static** `struct ramfs_file alias`
(ramfs.c:50) whose fields it overwrites on each call — non-reentrant,
and only safe because no caller holds the result across another lookup.

The cost is paid at four call sites, each doing the same two-step dance
(`sys_openat`, `sys_newfstatat`, `sys_faccessat`, `process_execve`):

```c
struct ramfs_dynamic_file *dyn = ramfs_dynamic_lookup(path);
if (dyn) { ...use dyn->data/dyn->size... }
const struct ramfs_file *file = ramfs_lookup(path);
if (file) { ...use file->data/file->size... }
```

**Simplification.** Have the applet lookup resolve to the underlying
`busybox` dynamic file and return `struct ramfs_dynamic_file *`. Then
`struct ramfs_file`, the static alias, and all four dual-lookup sites
collapse to a single `ramfs_resolve(path)`. That cascades usefully:
`fd_entry`'s `data`/`size` fields exist *only* to hold fixed-table file
bytes, so they can go too — simplifying `read_from_fd_entry`,
`sys_lseek`, `sys_fcntl`, and every one of the §4 copy sites. Estimated
**~60 lines removed** across four files, plus one non-reentrancy hazard
eliminated.

## 8. `paging_init()` maps 128MB one 4KB page at a time

`mm/riscv64_paging.c:351`:

```c
for (unsigned long addr = 0x80000000UL; addr < mem_top; addr += PAGE_SIZE)
        paging_map_page(addr, addr, PTE_PRESENT | PTE_WRITABLE);
```

That is **32,768 iterations**, each doing a full three-level walk and —
because `root == active_root` at that point — an `sfence.vma`. It also
allocates every intermediate table for the whole range.

Sv39 supports leaf PTEs at level 1 (2MB megapages). The identity map is
exactly the case they exist for: 64 megapage entries instead of 32,768
4KB entries, no level-0 tables, no per-page fence. This meaningfully
shortens boot under the interpreted emulator (where boot time is a
tracked budget) and removes the intermediate-table allocations. The
change is contained to `paging_init` plus megapage awareness in
`get_pte_in`.

Related, smaller: `paging_map_page_in()` and `paging_get_phys_in()` are
declared in `mm/paging.h` but have **zero callers outside
`riscv64_paging.c`** (verified). They can lose their external linkage.

## 9. Comment archaeology outweighs code in the headers

Comment-to-code ratios, measured:

| File | Total | Comment | Code | Comment % |
|---|---:|---:|---:|---:|
| `arch/riscv64_memmap.h` | 144 | 118 | 14 | **81%** |
| `sched/process.h` | 213 | 124 | 65 | **58%** |
| `mm/riscv64_paging.c` | 377 | 169 | 174 | 44% |
| `sched/riscv64_process.c` | 810 | 298 | 444 | 36% |
| `arch/riscv64_syscall.c` | 1,248 | 323 | 828 | 25% |

The phrase `checkpoint <N>` appears **124 times** across the kernel.
This is a genuine strength of the project — the "what broke and how it
was diagnosed" record is the point, and most of these comments explain
real bugs. But two distinct kinds of comment are currently mixed
together:

- **Design rationale** ("2MB sharing granularity, not 1GB, because
  process stacks alias otherwise") — belongs exactly where it is.
- **Development chronology** ("checkpoint 12: bumped from 16 to 512…",
  "was 0x80300000, a generous 1MB…") — describes a state the reader
  cannot see and will never encounter.

The second kind is what makes `process.h` 58% comment. Moving the
chronology to `docs/` (where the findings documents already live) and
keeping the rationale in-tree would roughly halve the header comment
volume without losing a single explanation of *why the code is the way
it is*.

**One stale comment worth fixing regardless** —
`arch/riscv64_syscall.c:18-27` still says:

> `brk_current`/`next_mmap_addr` below are still the single "one ring3
> context at a time" file-static globals… two real processes both calling
> `malloc()` would corrupt each other's heap bookkeeping… **Needs fixing
> before any real multi-process binary that mallocs runs concurrently.**

That was fixed — brk/mmap are per-process now (`process_current_brk`,
`process_take_mmap`), and `docs/self-hosting-todo.md` records it as a
blocker found and resolved during the self-hosting work. The file header
still describes the old broken design as current.

## 10. Smaller, mechanical items

- **`sys_openat` allocates an fd four times.** riscv64_syscall.c:536–638
  has four ~12-line blocks that each do `process_fd_alloc()` → null check
  → populate six `fd_entry` fields → `r->a0 = idx + 3`. One
  `alloc_fd_for(...)` helper removes ~35 lines.
- **`fill_dirent64` computes `reclen`, then `sys_getdents64` recomputes
  the identical expression inline** (`(19 + namelen + 1 + 7) & ~7u`) to
  do its buffer-space check. Two copies of one ABI detail.
- **The staircase indentation** in the `#define SYS_*` block
  (riscv64_syscall.c:35–69) and the `syscall_dispatch` switch (:1197–1237)
  ramps whitespace to ~100 columns for no semantic reason. Harmless, but
  it makes both blocks hard to scan and will fight any future edit.
- **`elf_load32`/`elf_load64`** (mm/elf.c) are ~33 near-identical lines
  each, differing only in header struct, `e_machine` constant, and a
  cast. Worth merging only if i386 stays (§11).
- **Unused defines**: `MAP_ANON` and `O_RDWR` in riscv64_syscall.c are
  defined and never referenced.
- **`syscall_init()` prints a 32-name syscall list** that must be
  hand-edited whenever a syscall is added — it has already drifted (the
  list omits `getuid`/`getgid`/`getegid`, which are implemented). Derive
  it from the dispatch table or drop it.
- **Duplicated QEMU invocation**: the same six `--qemu-arg` lines appear
  in `test`, `test-initrd`, and `test-selfhost` (Makefile). One variable.
- **`selfhost-initrd` and `tcc-initrd` are the same rule** producing two
  identically-built tarballs under different names (Makefile:377), and
  the rule runs `$(MAKE) -C ../compiler clean` — wiping the compiler
  build as a side effect of a kernel test target.
- **The initrd file list is written three times**: `RISCV64_USER_PROGRAMS`
  in the Makefile, and twice inside `test/build-initrd.sh` (once in the
  copy loop, once in the `tar cf` argument list).
- **Static footprint**: `struct process` is ~14.8KB (an inline 8KB kernel
  stack plus 32 × 176-byte `fd_entry`), so the process table is ~115KB
  of BSS; the ramfs table adds ~80KB. Fine at 128MB, but `MAX_FDS 32`
  with a 128-byte `path` in *every* entry — when only directory fds use
  `path` — is the bulk of it.

## 11. The i386 asymmetry is now the clearest remaining inconsistency

> **Corrected 2026-08-25, after empirical testing — read §12 first.**
> This section's framing ("archive vs. convert vs. freeze") assumed the
> i386 and riscv64 sides had genuinely diverged in capability. Measurement
> shows most of the gap is *labelling*, not architecture. The option list
> below is still valid but incomplete; §12 adds the option that should
> actually be taken.

The riscv64 side no longer embeds a single payload byte. The i386 side
still does:

- `kernel/hello_elf_payload.h` — 3,696 lines of generated C
- `kernel/user_test_payload.h` — 18 lines
- `kernel/gen_user_test_header.py` — 40 lines, now used **only** by i386

Both are `#include`d by `kmain.c` and compiled into the i386 kernel. So
3,714 of the tree's 11,617 lines — **32%** — are a generated artifact for
the secondary target, using a mechanism the primary target has already
retired.

This is the same "decide explicitly whether i386 stays" question the
repo-wide review raised, now with a sharper edge: i386 is not merely a
maintenance tax, it is the only remaining consumer of a pattern this
project has deliberately moved away from. Three coherent options, in
order of preference:

1. **Archive i386** to a branch/tag — removes ~6,800 lines (3,105 source
   + 3,714 generated) and makes `kernel/` single-target.
2. **Give i386 the same initrd treatment** — removes 3,714 generated
   lines and deletes `gen_user_test_header.py`, keeping both targets.
3. **Keep it and say so** — document i386 as a frozen reference
   implementation so the asymmetry reads as intentional.

What should *not* happen is leaving it undecided, because the two targets
now demonstrate contradictory conventions for the same problem.

## 12. Correction: most "riscv64-only" code is already architecture-neutral

**This supersedes the framing of §11, and partly of §1.**

The original draft of this review inferred the i386/riscv64 gap from the
two `OBJS` lists in the Makefile and from filenames. That was wrong, and
the error ran in one direction: it made the two targets look far more
divergent than they are. Re-checked by compiling and booting rather than
reading, at commit `15ea1e7`.

**What was verified empirically:**

- `make ARCH=i386 test` — **passes, 25/25 assertions**, clean boot under
  QEMU through `P5 checkpoint 2 OK`. i386 is not broken.
- `make TARGET=i386 selfcheck` — **passes**. stage1 is a real
  `ELF 32-bit LSB executable, Intel 80386, statically linked`, runs under
  `qemu-i386-static`, correctly compiles and runs all three regression
  programs. TCC genuinely self-hosts for i386.
- **`mm/ramfs.c` and `mm/tar.c` compile clean for i386, unmodified** —
  producing valid `ELF 32-bit LSB relocatable, Intel 80386` objects.

That last point is the correction. The RAM disk is **not** architecture
dependent:

- Every occurrence of the string `riscv64` in `mm/ramfs.c`, `mm/tar.c`,
  `mm/ramfs.h` and `mm/tar.h` is **inside a comment**.
- They already live in the shared `mm/` directory, alongside `mm/pmm.c`
  and `mm/elf.c`, which *are* compiled into both kernels today.
- They depend only on `pmm_alloc_contiguous()`, `pmm_free_page()` and
  `PAGE_SIZE`.

They are missing from the i386 kernel purely because nobody added two
entries to a Makefile variable. That is ~493 lines wrongly counted as
"needs porting" in §11's arithmetic.

The same mislabelling runs deeper:

| File | Code lines | Lines touching arch state | Arch-specific |
|---|---:|---:|---:|
| `sched/riscv64_process.c` | 444 | 25 | **~6%** |
| `arch/riscv64_syscall.c` | 828 | 150 (mostly arg marshalling) | ~18% |
| `mm/ramfs.c` + `mm/tar.c` | 260 | 0 | **0%** |

In `sched/riscv64_process.c`, the genuinely arch-dependent parts are the
trapframe save/restore, `satp` activation, initial `sstatus`/entry/sp
construction, `riscv64_trap_return`, and the hand-built context-switch
frame. Everything else — the process table, `alloc_slot`, round-robin
scheduling, `wait4`, the entire fd table, brk/mmap accounting, cwd
tracking, the drain hook — is portable C living in a file with `riscv64`
in its name.

In `arch/riscv64_syscall.c`, the 150 arch-touching lines are almost
entirely the boilerplate top-and-tail of each handler
(`unsigned long fd = r->a0;` … `r->a0 = ret;`). Comparing the two
implementations shows the entire architectural difference in the syscall
layer is three things:

| | i386 | riscv64 |
|---|---|---|
| syscall number | `r->eax` | `r->a7` |
| arguments | `r->ebx, ecx, edx, esi, edi, ebp` | `r->a0`–`r->a5` |
| return value | `r->eax` | `r->a0` |

Plus the syscall *numbers* themselves, which genuinely do differ per
architecture in the Linux ABI. The handler **bodies** — path resolution,
ramfs lookup, `dirent64` formatting, fd allocation, iovec walking — are
arch-neutral already.

Two further facts that change §11's cost estimate: `musl-i386` is built
and present, and `demo/build-busybox-i386.sh` (165 lines) plus a 45KB
i386 BusyBox compat patch already exist — an i386 BusyBox was built and
tested at some point in this project's history. It simply isn't built in
the current sandbox.

**Why the tree looks more divergent than it is.** Two conventions encode
"which architecture happened to need this first" as though it were
"which architecture this depends on":

1. **Filenames.** `riscv64_process.c` for 94%-generic code;
   `riscv64_syscall.c` for a file whose handler bodies are portable.
2. **The `OBJS` lists.** Membership is historical, not derived from
   actual dependencies — which is exactly how `mm/ramfs.o` and `mm/tar.o`
   ended up excluded from a build they compile fine in.

There is also precedent in-tree for doing this properly: `mm/pmm.c`,
`mm/elf.c` and `drivers/kprintf.c` are built for **both** targets with
**zero** `#ifdef`s between them (verified). `mm/elf.c` even handles both
ELF32 and ELF64 by branching at runtime on `e_ident[EI_CLASS]` rather
than at compile time. The codebase already knows how to do this; the
process and syscall layers just never got the same treatment.

**Consequence for §11.** The realistic option list gains a fourth entry,
which should be preferred over all three of the originals:

4. **Separate architecture from generic properly** — move the genuinely
   arch-dependent code into `arch/i386/` and `arch/riscv64/` behind a
   small explicit interface, and let everything else be built once for
   both. This makes i386's actual gap visible and small instead of
   apparent and large, and it removes the need to decide "archive or
   convert" under a false premise. See
   [`kernel-arch-split-plan.md`](kernel-arch-split-plan.md).

**What i386 still genuinely lacks** (unchanged by this correction, and
real work regardless of directory layout): a page-fault-driven COW/stack
growth path equivalent to riscv64's, the arch half of process creation
and context switching wired to `int 0x80`/`iret`, the ~25 additional
syscall *numbers*, and an initrd hand-off (QEMU's `-device loader` has no
Multiboot equivalent — the Multiboot module mechanism is the natural fit,
and it is a genuinely different mechanism, not just a different address).
That is a real port. It is just much smaller than 1,500 lines, and none
of it is the filesystem.

---

## Suggested order

Ranked by benefit against risk of disturbing the just-completed
self-hosting work:

1. **§9's stale comment** and **§10's unused defines / drifted syscall
   banner** — minutes, zero risk, and the stale comment actively
   misinforms.
2. **§5 `pmm_alloc_zeroed_page()`** and **§4 `fd_entry_copy()`** — small,
   local, each closes a real repetition-driven bug class.
3. **§6 ramfs hot path** — no interface change, directly speeds up the
   self-hosting test that currently exercises it hardest.
4. **§1 + §2: invert the boot/test relationship** — the highest-value
   change in this document. Best done as one commit, since the pieces
   (drain hook, `kernel.h` declarations, checkpoint strings) only come
   out cleanly together.
5. **§3 shared process-image builder** — largest line reduction; do it
   after §1 so `run_elf_test` is already gone rather than a third case
   to accommodate.
6. **§7 collapse `ramfs_file`** — pleasant cascade into `fd_entry`, but
   touches four syscall paths, so it wants the test suite stable first.
7. **§8 megapage identity map** — isolated and valuable for emulator boot
   time, but it is the one item here that changes real MMU behavior;
   sequence it alone, verified on both QEMU and the Wasm emulator.
8. **§12's arch/generic split** — see
   [`kernel-arch-split-plan.md`](kernel-arch-split-plan.md). Sequence it
   *after* §1 and §3: those two delete or merge a lot of the code that
   would otherwise have to be classified and moved, so doing them first
   makes the split strictly smaller. Its phase 1 (moving `mm/ramfs.o` and
   `mm/tar.o` into the i386 build, which needs no code changes at all)
   can be done at any time.
9. **§11 the i386 decision** — a judgement call, not a refactor, and one
   that §12 shows was being made on bad information. Settle it *after*
   the split makes the true remaining gap visible.

Rough total for items 1–7, excluding the arch split and the i386
decision: **~400 lines removed**, no behavior change, and three
subsystems (syscall dispatch, scheduler, `kernel.h`) stop depending on
the test suite.
