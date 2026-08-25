# RV64 emulator

The active emulator core is standard C in `wasm/rv64.c`, compiled by this
repository's TCC wasm32 target. It implements the RV64IM, Sv39, supervisor,
Sstc, UART, and small F/D subsets used by this repository. Wasm supplies
native 64-bit integer registers; there is no JavaScript `BigInt` CPU loop.

The browser wrapper in `js/worker.js` only instantiates Wasm, copies ELF load
segments into guest RAM, forwards terminal input, drains UART output, and
yields between execution batches.

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

The former plain-JavaScript implementation remains in `js/cpu.js`,
`js/mmu.js`, `js/memory.js`, and `js/uart.js` as a readable reference and
comparison implementation; the browser worker no longer loads it.
