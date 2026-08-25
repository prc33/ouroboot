# RV64 emulator

The active emulator core is standard C in `wasm/rv64.c`, compiled by this
repository's TCC wasm32 target. It implements the RV64IM, Sv39, supervisor,
Sstc, UART, and small F/D subsets used by this repository. Wasm supplies
native 64-bit integer registers; there is no JavaScript `BigInt` CPU loop.

The browser wrapper in `js/worker.js` only instantiates Wasm, copies ELF load
segments into guest RAM, forwards terminal input, drains UART output, and
yields between execution batches.

`wasm/runner.c` is the alternative native front end. It is a small ELF loader
and stdin/stdout UART bridge around the exact same C core. In an i386 system
with this repository's TCC installed, build and run it from a command prompt:

```sh
cd emulator/wasm
tcc -O2 -o rv64-run runner.c
./rv64-run ../../kernel/kernel-shell.elf
```

On a development host, `make native` does the same using
`../../compiler/tcc`; build that compiler with `TARGET=i386` first. An optional
second argument limits the instruction count, which is useful for scripted
runs. With no limit the emulator remains attached to the terminal until it is
interrupted.

Build and test:

```sh
make -C compiler TARGET=wasm32
make -C emulator/wasm
make -C emulator/wasm test
make -C emulator/wasm test-boot
make -C emulator/wasm test-shell
```

For the browser demo, build `kernel/kernel.elf`, serve the repository root,
and open `http://localhost:8000/emulator/js/`. The generated
`emulator/js/rv64.wasm` has no WASI or JavaScript imports.
