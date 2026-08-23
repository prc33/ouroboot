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
  `qemu-riscv64-static` — see `docs/riscv-port-findings.md` for four
  real bugs found and fixed along the way.
- **TCC self-hosted for riscv64 (compiled by itself, then actually
  run) is NOT yet working** — it hangs. This is a distinct, narrower
  gap from the host-tool capability above; see the last section of
  `docs/riscv-port-findings.md`.
- **`kernel/` is still the original i386 kernel.** A RISC-V64 kernel
  port has not been started — no Multiboot equivalent exists for
  RISC-V, the boot/privilege/interrupt model is entirely different
  (OpenSBI/SBI, CLINT/PLIC, no segmentation at all), and this is
  genuinely comparable in scope to the whole original i386 kernel
  effort, not a small follow-on.

## Layout

- **`compiler/`** — TCC, stripped to exactly two targets (i386,
  riscv64), one source tree, target selected at build time
  (`make TARGET=i386` or `make TARGET=riscv64`). No other backends,
  no Windows/macOS output formats, no bounds checking. riscv64 has no
  integrated assembler for real instructions — replaced by compiler
  intrinsics; see `docs/riscv-port-findings.md` for why that turned
  out to be sufficient (and why it wasn't as simple as first assumed).

- **`kernel/`** — the i386 kernel, from scratch: Multiboot boot,
  serial console, GDT/IDT/PIC/PIT, physical memory allocator, paging
  with copy-on-write, a two-task cooperative scheduler, ring0→ring3
  transition, a real ELF loader. `make test` boots it under QEMU and
  asserts on the serial transcript — see `docs/kernel-p3-findings.md`
  onward for the testing philosophy and every bug found along the way.

- **`demo/`** — proof that the compiler produces correct, runnable
  binaries against real-world software, for *both* targets. Each
  script clones real upstream musl/busybox fresh (not a vendored
  copy), applies a patch from `demo/patches/`, and builds+runs a
  comprehensive test. Deliberately kept as scripts against a small
  patch rather than a full vendored fork — see `demo/patches/` for
  why (a few files changed against tens of thousands of lines of
  upstream).

- **`emulator/`** — not started. See `emulator/README.md`.

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
cd ../kernel && make test            # i386 only, for now
```
