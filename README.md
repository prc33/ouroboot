# self-hosting-system

A from-scratch self-hosting system: a compiler (stripped TCC), a
kernel, and (eventually) an emulator and FPGA implementation for the
same target architecture, such that the compiler can rebuild itself
while running under the kernel while running on the emulator/FPGA.

## Status (honest summary — read this before the subdirectories)

**i386: the closure milestone is met.** `compiler/` (built with
`TARGET=i386`) self-hosts, and a real static musl+TCC binary runs
under `kernel/` via a real ELF loader, real ring0→ring3 transition,
real syscalls — verified end to end, not assumed. See
`docs/kernel-p5-checkpoint2-findings.md`.

**The project has since pivoted to RISC-V64** as the primary target
(driven by wanting a simpler instruction encoding for a future
emulator/FPGA implementation — see the plan doc and findings for the
reasoning). For RISC-V64:

- `compiler/` (built with `TARGET=riscv64`) correctly compiles
  target code as a **host tool** — fully proven, this is what built
  and ran the entire musl and busybox test suites below.
- musl and busybox both build and run correctly under
  `qemu-riscv64-static` — see `docs/riscv-port-findings.md` for the
  real bugs found and fixed along the way.
- **TCC self-hosted for riscv64 (compiled by itself, then actually
  run) now works** — `make TARGET=riscv64 selfcheck` passes
  end-to-end, the same bar as i386. This closes the riscv64 closure
  milestone; see `docs/riscv-port-findings.md`'s closure section for
  the bug that was blocking it (a codegen bug in variadic-function
  epilogues, unrelated to self-hosting specifically).
- **`kernel/` now has a full RISC-V64 port, alongside the untouched
  i386 kernel** (`make ARCH=riscv64 test`, mirroring `make TARGET=`'s
  shape) — boot under OpenSBI, trap/exception handling, Sv39 paging
  with copy-on-write, an Sstc-timer-driven scheduler, S-mode→U-mode
  transitions with real `ecall` syscalls, and a real ELF loader
  running an actual static musl+TCC riscv64 binary in userspace, same
  closure-adjacent bar as i386's own P5 checkpoint 2. Every i386 file
  is untouched; see `docs/riscv-port-findings.md`'s kernel-port
  section for the design (raw machine code where TCC's assembler has
  no relocation support at all, real bugs found booting each stage).
  A writable ramfs (real `open(O_CREAT)`/`write`/`unlink`) and a real
  tar-loading initrd (`make ARCH=riscv64 test-initrd`) followed.

- **Not yet done: TCC running *inside* this kernel** (as opposed to
  under `qemu-riscv64-static`'s Linux user-mode emulation, which
  `make TARGET=riscv64 selfcheck` above already proves). This is the
  project's actual closure condition — see
  `docs/self-hosting-todo.md` for exactly what's been ruled out, the
  one open bug blocking it (a real, deterministic page fault giving a
  process a bigger stack), and the concrete next steps. Read that
  file before touching `mm/ramfs.*`'s dynamic-file matching,
  `sched/riscv64_process.c`'s `execve()` stack layout, or
  `arch/riscv64_memmap.h`'s initrd size constants.

## Layout

- **`compiler/`** — TCC, stripped to exactly two targets (i386,
  riscv64), one source tree, target selected at build time
  (`make TARGET=i386` or `make TARGET=riscv64`). No other backends,
  no Windows/macOS output formats, no bounds checking. riscv64 has no
  integrated assembler for real instructions — replaced by compiler
  intrinsics; see `docs/riscv-port-findings.md` for why that turned
  out to be sufficient (and why it wasn't as simple as first assumed).

- **`kernel/`** — from scratch, both targets: i386 (Multiboot boot,
  serial console, GDT/IDT/PIC/PIT) and riscv64 (OpenSBI boot, MMIO
  serial console, a single unified trap vector instead of an IDT, Sstc
  timer instead of PIC/PIT). Both get: physical memory allocator,
  paging with copy-on-write (2-level i386 / Sv39 riscv64), a two-task
  cooperative scheduler, ring0→ring3 (i386) / S-mode→U-mode (riscv64)
  transition, a real ELF loader. `make test` (i386) / `make
  ARCH=riscv64 test` boots each under QEMU and asserts on the serial
  transcript — see `docs/kernel-p3-findings.md` onward for i386's
  testing philosophy and every bug found along the way, and
  `docs/riscv-port-findings.md`'s kernel-port section for riscv64's.

- **`demo/`** — proof that the compiler produces correct, runnable
  binaries against real-world software, for *both* targets. Each
  script clones real upstream musl/busybox fresh (not a vendored
  copy), applies a patch from `demo/patches/`, and builds+runs a
  comprehensive test. Deliberately kept as scripts against a small
  patch rather than a full vendored fork — see `demo/patches/` for
  why (a few files changed against tens of thousands of lines of
  upstream).

- **`emulator/`** — a from-scratch RISC-V emulator (plain JS, jor1k as
  reference not a fork base). `emulator/js/` boots `kernel/kernel.elf`
  both headlessly (`make ARCH=riscv64 test-js`, passing every
  checkpoint `make ARCH=riscv64 test`/QEMU does) and in an actual
  browser tab (`xterm.js` + a Web Worker, verified against real
  headless Chromium). Real userspace binaries (needs float/double
  support) are next. See `emulator/README.md` and
  `docs/emulator-plan.md`.

- **`cpu/`** — not started (FPGA implementation, future work). See
  `cpu/README.md`.

- **`docs/`** — the plan, and a findings doc per major phase/port,
  each documenting not just what was built but what broke, how it was
  diagnosed, and how it was fixed. This is where the actual narrative
  lives; the code and patches are the minimal proof.

## Quick start

```
cd compiler && make TARGET=i386      # or TARGET=riscv64
cd ../demo && ./build-musl-i386.sh   # or -riscv64
             ./build-busybox-i386.sh # or -riscv64
cd ../kernel && make test            # or make ARCH=riscv64 test
```

## Current all-in demo: a real interactive shell, in a real browser tab

The riscv64 kernel, running under the from-scratch JS emulator
(`emulator/`), in an actual browser tab -- not just Node or QEMU --
boots all the way to a real busybox ash prompt that accepts real
typed input:

```
cd kernel && make ARCH=riscv64           # build kernel.elf
cd ..                                    # repo root -- index.html
                                          # fetches ../../kernel/kernel.elf,
                                          # so the server has to be rooted
                                          # here, not in emulator/js/
python3 -m http.server 8000
# open http://localhost:8000/emulator/js/index.html
```
(Any static HTTP server works -- `fetch()` and Worker script loading
both need a real origin, not `file://`.)

You'll see the kernel boot, run its pmm/paging/COW/scheduler/syscall
checkpoints, load a real static musl+TCC riscv64 binary into userspace
via a real ELF loader, fork/exec/wait4 real child processes, and
finally hand off to a real embedded busybox `ash` -- all inside
`xterm.js`, with the CPU running in a Web Worker so it never blocks
the UI. Once you see the `#` prompt, click into the terminal and
type: real keystrokes go out over the same UART byte interface the
kernel's own serial console uses. (The boot sequence itself is CPU-
bound JS interpretation of the whole checkpoint 1-10 test suite, so
reaching that prompt takes real wall-clock minutes, not seconds --
patience, not a hang.)

Headless equivalent (same checkpoints, including a scripted shell
session, asserted against QEMU's own output so the two can't silently
drift apart): `cd kernel && make ARCH=riscv64 test-js`. See
`emulator/README.md` for both in more detail.
