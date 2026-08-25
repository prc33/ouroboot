# Ouroboot RV64 emulator

This directory is a complete RV64 emulator with two front ends:

- a browser terminal, with the C core compiled to WebAssembly;
- a native command-line program, with the same core compiled for i386.

The core is the standard C file `rv64.c`. It implements the RV64IM
instructions, Sv39, supervisor mode, Sstc, a 16550-style UART, and the small
F/D subset needed by Ouroboot. Both front ends load the same RISC-V ELF kernel
and use the same UART interface; neither contains another CPU implementation.

## Build the guest

From the repository root, build the RISC-V compiler and the kernel that boots
directly into BusyBox `ash`:

```sh
make -C compiler clean
make -C compiler TARGET=riscv64
make -C kernel ARCH=riscv64 kernel-shell.elf
make -C kernel ARCH=riscv64 tcc-initrd
```

Use `kernel/kernel.elf` instead if you want the longer boot containing every
historical test checkpoint before the shell. The compiler build must be
cleaned when changing targets because its targets share generated filenames.

## Browser demo

Build the freestanding Wasm module with this repository's wasm32 TCC backend:

```sh
make -C compiler clean
make -C compiler TARGET=wasm32
make -C emulator web/rv64.wasm
```

Then serve the repository root and open the terminal page:

```sh
python3 -m http.server 8000
```

Open <http://localhost:8000/emulator/web/>. By default it loads the direct-shell
kernel and `kernel/tcc-initrd.tar`, so `ls` includes the runnable `tcc`.
Use `?kernel=../../kernel/kernel.elf` to run every historical checkpoint first.
To expose the complete source/header closure image instead, build
`selfhost-initrd` and add `?initrd=../../kernel/selfhost-initrd.tar`.
The page obtains xterm.js from a CDN, so that first load needs network access.
It starts the emulator in a Web Worker, leaving the terminal responsive while
the guest runs. `fetch()` and Worker loading require HTTP; opening the HTML as
a `file://` URL will not work.

The `kernel` and `initrd` query parameters can select any served ELF and tar.
Both are mandatory boot artifacts; the kernel contains no built-in filesystem.
The generated `web/rv64.wasm` is freestanding and has no WASI or JavaScript
imports.

## Native command-line demo

The small `runner.c` front end loads ELF segments and the initrd, then bridges the
emulated UART to standard input and output. Inside an i386 POSIX VM containing
this project's TCC, a libc such as musl, and the repository, build and run it
with:

```sh
cd emulator
tcc -O2 -o rv64-run runner.c
./rv64-run ../kernel/kernel-shell.elf ../kernel/tcc-initrd.tar
```

From the repository root, the equivalent build is `make -C emulator
native`; it uses `compiler/tcc`, which must have been built with `TARGET=i386`
and must have an i386 libc available for linking. A normal host C compiler can
also build the runner for quick development (`cc -O2 -o rv64-run runner.c`).

The runner stays attached to the terminal until interrupted. An optional third
argument limits executed instructions, for example
`./rv64-run ../kernel/kernel.elf ../kernel/initrd.tar 100000000`.

## Automated checks

With Node.js installed, the emulator checks are:

```sh
make -C emulator test-boot   # boot kernel/kernel.elf
make -C emulator test-shell  # scripted BusyBox shell session
make -C emulator test        # run both
```

`test-boot` and `test-shell` expect their kernel ELF and `kernel/initrd.tar` to
have already been built. The repository-level regression test is:

```sh
make -C kernel ARCH=riscv64 test-wasm
```

It boots through every kernel checkpoint, drives `ash`, checks filesystem
writes, and requires the P10 completion marker.

## Layout

- `rv64.c` — shared emulator core and UART queues.
- `runner.c` — native ELF loader and terminal front end.
- `test/boot.mjs` — headless Node.js ELF loader and test runner.
- `web/worker.js` — browser ELF loader and execution worker.
- `web/app.js`, `web/index.html` — xterm.js terminal page.

Generated files (`web/rv64.wasm` and `rv64-run`) are removed by
`make -C emulator clean`. The independent wasm32 compiler examples and tests
live in `compiler/tests/wasm32`.
