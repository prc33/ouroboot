# Ouroboot

Ouroboot is a small, understandable, self-hosting computer system. A compact C
compiler builds musl, BusyBox, a kernel, and another copy of itself. A compact
RV64 emulator—also written in C and compiled to WebAssembly by that compiler—
runs the result in a browser terminal.

The goal is not to reproduce Linux or provide a production toolchain. It is to
keep the complete path from C source to an interactive, self-compiling system
small enough to read:

```text
host C compiler -> TCC -> musl + BusyBox + kernel + guest TCC
                         |
TCC wasm32 -> RV64 emulator.wasm -> browser -> BusyBox ash -> TCC rebuilds TCC
```

RISC-V64 is the primary system. The compiler and kernel retain an i386 port as
a second implementation and self-hosting check. The wasm32 compiler target is
deliberately freestanding and writes a final module directly; it is primarily
used to build the emulator.

## Line-count budgets

These limits count tracked implementation, build, and test files inside each
subdirectory; subsystem README prose is excluded. Generated files and ignored
upstream build trees are not part of the repository count.

| Component | Limit or target | Current | Status |
|---|---:|---:|---|
| Emulator | 1,000 hard limit | 939 | Within limit |
| Kernel | 10,000 hard limit | 9,510 | Within limit |
| Compiler | 25,000 target; 20,000 stretch | 30,428 | Not yet attained |

New functionality must stay within the emulator and kernel limits. Compiler
work should reduce the current count while preserving standard C, ELF output
for i386/RISC-V64, and the freestanding wasm32 backend.

### Compiler interface cleanup

This change reduces compiler source by 21 lines overall (30,449 to 30,428).
The new headers replace broad `tcc.h` inclusion with the smallest shared
platform and target-definition interfaces. `registers.c` deliberately remains
on `tcc.h`: its code-generation/backend boundary has not yet been extracted.

| File | Before | After | Change | Responsibility |
|---|---:|---:|---:|---|
| `common.h` | — | 48 | +48 | shared C/platform definitions |
| `target.h` | — | 20 | +20 | selected target definitions |
| `types.c` / `types.h` | 585 | 592 | +7 | C type rules and API |
| `vstack.c` / `vstack.h` | 168 | 172 | +4 | value stack and temporary locals |
| `symbols.c` / `symbols.h` | 565 | 474 | -91 | symbol scopes and attributes |
| `tccelf.c` / `tccelf.h` | 2,506 | 2,591 | +85 | ELF and archive format/API |
| `tcctools.c` | 317 | 270 | -47 | archive writer |
| `tcc.c` | 1,553 | 1,580 | +27 | compiler driver and dependency output |
| `tcc.h` | 980 | 905 | -75 | remaining compiler-wide state/API |
| `registers.h` | 14 | 15 | +1 | register allocator API |

## Quick start: build the browser system from scratch

You need a POSIX build environment with a host C compiler, Make, Git, Bash,
standard Unix build tools, Python 3, and a modern browser. The build scripts
fetch musl and BusyBox, so their first run needs network access. Node.js and
QEMU are only needed for the automated tests described below.

From the repository root:

```sh
# Build the RISC-V64 compiler and its runtime library.
make -C compiler TARGET=riscv64

# Fetch, patch, and build musl and BusyBox with that compiler.
./demo/build-musl-riscv64.sh
./demo/build-busybox-riscv64.sh

# Build the kernel and an initrd containing BusyBox, TCC, its source, musl,
# hello.c, and the self-hosting demonstration script.
make -C kernel ARCH=riscv64 kernel.elf tcc-initrd

# Build the C RV64 emulator as a freestanding Wasm module using TCC itself.
make -C compiler TARGET=wasm32
make -C emulator

# Serve the repository root; fetch() and Web Workers do not work via file://.
python3 -m http.server 8000
```

Open <http://localhost:8000/emulator/web/>. After BusyBox reaches the `#`
prompt, try:

```sh
ls
cat hello.c
ash /selfhost.sh
```

The script compiles and links a new TCC from the source files in `/tcc-src`,
uses it to build `/hello.c`, and runs the resulting program. Compilation inside
the instruction-interpreting browser emulator is intentionally much slower
than booting the shell.

For the same full self-hosting proof under QEMU:

```sh
make -C kernel ARCH=riscv64 test-selfhost
```

This requires `qemu-system-riscv64`. Compiler-only closure tests use
`qemu-riscv64-static` and `qemu-i386-static`:

```sh
make -C compiler TARGET=riscv64 selfcheck
make -C compiler TARGET=i386 selfcheck
```

## How it works

The compiler is a reduced TinyCC with three build-time targets. Its frontend,
preprocessor, ELF machinery, and command-line driver are distinct compilation
units shared by the targets. The i386 and RISC-V64 backends emit ELF; their
small architecture-specific linker files implement the relocation and ABI
rules that cannot be shared. The wasm32 backend emits a final freestanding
module without introducing a Wasm object format or general-purpose linker.

The kernel is standard C plus small architecture entry/context-switch files.
It provides paging with copy-on-write, processes, an ELF loader, a writable
RAM filesystem populated from an explicit tar initrd, and the POSIX syscall
surface needed by static musl, BusyBox `ash`, and TCC. It is not Linux: musl and
BusyBox are ordinary userspace programs running on Ouroboot's own kernel.

The emulator core is one standard-C RV64IM implementation with Sv39, supervisor
mode, Sstc timers, a UART, and the small floating-point subset used by the
guest. The same core has two frontends:

- TCC compiles it to Wasm for a browser Web Worker; a small JavaScript layer
  connects its UART to xterm.js and loads the kernel ELF and initrd.
- A native C harness can run the same artifacts from a terminal; see
  [`emulator/README.md`](emulator/README.md).

`demo/` is build infrastructure, despite its historical name. Its tracked
scripts fetch pinned upstream musl and BusyBox releases, apply the compatibility
patches in `demo/patches/`, and create the ignored top-level
`musl-<arch>/`/`busybox-<arch>/` build trees. The kernel and self-hosting initrd
consume those trees; musl and BusyBox are not compiled by some separate browser
pipeline. A local `demo/busybox/` directory is merely an ignored old working
tree and is not used.

## Repository map

- `compiler/` — reduced TCC and the i386, RISC-V64, and wasm32 backends.
- `demo/` — reproducible musl/BusyBox fetch, patch, build, and smoke-test scripts.
- `kernel/` — the i386/RISC-V64 kernel, userspace fixtures, initrd builders, and
  QEMU/browser regression tests.
- `emulator/` — the shared C RV64 core, native runner, browser frontend, and
  headless Node test harness.
- `docs/` — design history, investigations, and detailed subsystem findings.
- `cpu/` — reserved for a possible future FPGA implementation.

Generated dependency trees and binaries are ignored rather than vendored. The
component Makefiles provide clean targets; the four `demo/build-*.sh` scripts
deliberately recreate their upstream trees.

## Origins and influences

- [TinyCC](https://bellard.org/tcc/) is the compiler foundation. Ouroboot removes
  unused hosts and targets, adds RISC-V64 and wasm32 support, and keeps the
  compiler capable of standard C and separate compilation.
- [musl](https://musl.libc.org/) supplies the C library and startup objects used
  by guest programs. Its build also acts as substantial compiler validation.
- [BusyBox](https://busybox.net/) supplies the real `ash` shell and command-line
  utilities used as kernel workloads.
- [Blosc MiniCC](https://github.com/Blosc/minicc) provided the starting point
  for the small TinyCC wasm32 backend, which was adapted to this compiler.
- [mini-rv32ima](https://github.com/WillGreen/mini-rv32ima) demonstrated how
  compact a useful RISC-V interpreter can be and informed emulator review.
- [jor1k](https://github.com/s-macke/jor1k) was performance and architecture
  prior art for a browser-hosted machine, not a source-code dependency.
- [xterm.js](https://xtermjs.org/) provides the browser terminal UI; all machine
  execution remains in Ouroboot's C/Wasm emulator.
- [OpenSBI](https://github.com/riscv-software-src/opensbi) supplies the standard
  firmware environment used when the RISC-V kernel is tested under QEMU.

The upstream licenses and retained notices live with their respective source
trees. See the subsystem READMEs and `docs/` for implementation details and the
bugs found while reaching self-hosting closure.
