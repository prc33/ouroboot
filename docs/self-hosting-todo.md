# TODO: TCC compiling TCC, inside the running kernel

Handover checkpoint — where this stands, what's proven, what's next.
Read this before touching `mm/ramfs.*`, `sched/riscv64_process.c`'s
execve path, or `arch/riscv64_memmap.h`'s initrd constants.

## The actual goal

Not yet done: **run TCC as a process inside this kernel (real QEMU or
the JS emulator, not `qemu-riscv64-static`'s Linux user-mode
emulation) and have it compile something real — ideally itself**,
matching `docs/self-hosting-system-plan.md`'s own closure condition
("TCC compiles TCC, inside our kernel, inside our emulator").

What's proven today is *not* that: `make TARGET=riscv64 selfcheck`
(compiler/Makefile) proves TCC's own riscv64 codegen is correct —
stage1 (TCC compiled by itself) runs under `qemu-riscv64-static`, a
real Linux syscall-ABI emulator, entirely separate from this
project's own kernel.

## What this session did (all committed, all tested)

1. **`mm/ramfs.*`: dynamic (writable) files are now matched by full
   normalized path, not basename.** This was a real, confirmed
   blocker: musl-riscv64's own header tree relies on directory-
   priority overrides (`include/`, `arch/riscv64/`, `arch/generic/`),
   and there are real basename collisions between them —
   `bits/fenv.h`, `errno.h`, `fcntl.h`, `dirent.h`, `ioctl.h`, and
   ~25 more (confirmed empirically with `comm`/`find | sort | uniq
   -d` against a real musl-riscv64 checkout). A flat/basename-only
   ramfs silently picks the wrong one; full-path matching fixes this
   without needing real directory inodes (no mkdir, no readdir on
   subdirectories — TCC only ever does `open()`/`stat()` by
   constructed path, confirmed by reading how `-I` search actually
   works). `RAMFS_MAX_DYNAMIC_FILES` bumped 16 → 512 (musl's header
   tree alone is ~230 files, measured directly).
2. `PATH_MAX_LOCAL` (arch/riscv64_syscall.c) 64 → 128, and
   `RV64_INITRD_MAX_SIZE` (arch/riscv64_memmap.h) 4MB → 16MB, sized
   against a real measurement of what a self-hosting payload needs
   (TCC's own source ~1.1MB + musl headers ~1.1MB + `libc.a` 2.7MB +
   crt objects + `libtcc1.a` + a prebuilt riscv64 `tcc` binary
   ~280KB ≈ 5-6MB total).
3. `EXECVE_MAX_ARGV` 8 → 20 (both copies — arch/riscv64_syscall.c and
   sched/riscv64_process.c must match), sized against the real,
   proven invocation `compiler/Makefile`'s own `stage1:` recipe uses
   (13 argv entries for the compile step, 13 for the link step).
4. Full riscv64 test suite (`make ARCH=riscv64 test`, `test-initrd`,
   `test-js`, `test-initrd-js`) passes with all of the above. i386
   confirmed unaffected (`make test`, default ARCH). TCC restored to
   `TARGET=riscv64` as the final committed state.

## What was attempted and reverted (real, open bug — not silently dropped)

A real self-hosted TCC invocation needs two things this kernel's
`execve()` doesn't have yet, and they have to be solved *together*:

- **A much bigger per-process stack.** `sched/riscv64_process.c`'s
  `process_execve()` gives every process a fixed, non-growable 2-page
  (8KB) stack. TCC compiling its own ~15K-line unity-build source
  (`tcc.c` `#include`-ing `libtcc.c`/`tccpp.c`/`tccgen.c`/`tccelf.c`/
  `tccasm.c`/`riscv64-gen.c`/`riscv64-link.c`/`riscv64-asm.c`) is a
  genuinely deep recursive-descent parser; 8KB is a real risk of
  silent stack-into-heap corruption, not just theoretical headroom.
- **More argv room.** The real compile/link command lines (mirrored
  exactly from `compiler/Makefile`'s `stage1:` recipe — see below)
  need more than the 512 bytes `STRDATA_SIZE`+`PTRBLOCK_SIZE`
  currently reserve at the top of that same tiny stack.

Bumping `EXECVE_STACK_PAGES` to 256 (1MB) alone produced a real,
**deterministic** page fault inside busybox/ash's own code (read/exec
fault at a fixed stack address, same address and same `sepc` across
repeated runs) — not a flaky/uninitialized-memory issue. Explicitly
zeroing the newly allocated stack pages (the first, cheaper theory —
`pmm_alloc_page()` makes no zeroing guarantee, confirmed in
`mm/pmm.c`) was tried and did **not** fix it, so that's ruled out.

Current best guess, **not yet confirmed**: `sched/riscv64_process.c`'s
`FORK_STACK_HI` (`0xB0002000UL`, a comment right there says "2 pages,
matches process_create_from_elf's own stack_pages") still hardcodes
the *old* 2-page COW-clone window. If a process gets a bigger stack
via `process_execve()` and then `fork()`s (ash does fork around
running external commands — see that function's own comment on the
checkpoint 9 mmap bug, a similar shape), the child's COW clone would
only get the first 2 pages mapped, and any of the parent's real stack
usage above `0xB0002000` (which a 1MB stack makes far more likely to
exist) would page-fault the instant the child touches it. This needs
actually confirming (e.g., trace whether fork() is called at all
before the fault, or shrink `EXECVE_STACK_PAGES` incrementally to
find the exact breaking point) before trying the fix.

Growing `STRDATA_SIZE`/`PTRBLOCK_SIZE` alone (without the stack-size
fix) was also tried and separately produces a *different* fault: a
plain stack overflow (write fault just below `stack_va`) inside
busybox/ash's own startup — confirming real programs already use
most of the current 8KB budget, independent of the argv question.

**Current committed state**: both reverted to their original,
`process_create_from_elf`-matching values (`EXECVE_STACK_PAGES=2`,
`STRDATA_SIZE=256`, `PTRBLOCK_SIZE=256`). `EXECVE_MAX_ARGV=20`/
`EXECVE_ARG_MAX=128` were kept (harmless on their own — no current
test passes enough args to hit either ceiling) as headroom for
whoever picks this back up.

## Next steps, in order

1. **Root-cause the big-stack page fault.** Start with the
   `FORK_STACK_HI` theory above — grep for where `process_fork()` is
   actually invoked in the current test suite's ash session and
   confirm whether it runs before the fault. If confirmed, either
   make `FORK_STACK_HI` track the forking process's *actual* stack
   size (needs a real per-process "how big is my stack" field —
   `struct process` doesn't have one yet) or special-case a bigger
   COW-clone window for processes that came from `process_execve()`
   with a bigger stack.
2. Once a process can safely get a real (~1MB) stack *and* survive a
   fork afterward, re-apply the `STRDATA_SIZE`/`PTRBLOCK_SIZE` growth
   (2560+512 bytes was the earlier estimate for 20×128 argv) within
   that bigger budget.
3. **Package the actual self-hosting payload** into a tar for
   `mm/tar.c`'s existing loader (`kernel/Makefile`'s `test-initrd`
   pattern is the template — a new `test-selfhost`/`test-selfhost-js`
   pair should follow it exactly):
   - `compiler/*.c`, `compiler/*.h` (all of it — TCC's own quoted
     `#include "libtcc.c"`-style includes resolve relative to the
     including file's own directory, so these just need to live
     together in one directory; no `-I` needed for TCC's own files).
   - `compiler/include/*.h` (TCC's bundled freestanding headers —
     `float.h`, `stdarg.h`, `stddef.h`, etc.).
   - musl-riscv64's `obj/include/`, `include/`, `arch/riscv64/`,
     `arch/generic/` (this is exactly the part that needed the
     full-path-matching fix above — these directories have real
     basename collisions).
   - musl-riscv64's `lib/crt1.o`, `lib/crti.o`, `lib/crtn.o`,
     `lib/libc.a`.
   - `compiler/libtcc1.a` and a prebuilt riscv64 `compiler/stage1/tcc`
     binary (`make TARGET=riscv64 stage1` in `compiler/` first — this
     binary is what actually runs *inside* the kernel and does the
     compiling).
   - Exact proven invocation to mirror (`compiler/Makefile`'s own
     `stage1:` target, DEFS included — `-DTCC_TARGET_RISCV64
     -DCONFIG_TCC_ASM -DCONFIG_TRIPLET="riscv64-linux-musl"`):
     ```
     tcc -B/tcc -I/tcc -I/tcc/include -I/musl/obj/include \
         -I/musl/include -I/musl/arch/riscv64 -I/musl/arch/generic \
         -nostdinc -DTCC_TARGET_RISCV64 -DCONFIG_TCC_ASM \
         -DCONFIG_TRIPLET=\"riscv64-linux-musl\" \
         -c -o tcc_stage2.o /tcc/tcc.c
     tcc -B/tcc -static -nostdinc -nostdlib -o tcc_stage2 \
         /musl/lib/crt1.o /musl/lib/crti.o tcc_stage2.o \
         /musl/lib/libc.a /tcc/libtcc1.a /musl/lib/crtn.o
     ```
   - Expect real errors on the first attempts (this has never been
     tried) — debug them the same way every other checkpoint in this
     project has, against real QEMU serial output, not guessed.
4. Verify the resulting `tcc_stage2` actually runs (`./tcc_stage2 -v`
   at minimum) inside the kernel, and ideally that it can compile a
   trivial program correctly too (mirroring `selfcheck`'s own `int
   main(){return 42;}` check).
5. Add the permanent `test-selfhost`/`test-selfhost-js` Makefile
   targets once step 4 passes, so this can never regress silently —
   this was the user's explicit ask alongside the investigation
   itself ("ensure there is a 'full self-hosting' test case").
6. Stretch goal, matching `docs/emulator-plan.md`'s own P5 bar
   exactly: have `tcc_stage2` recompile TCC *again* and diff the two
   generations' output.
