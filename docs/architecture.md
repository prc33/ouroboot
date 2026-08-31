# Architecture

Ouroboot closes a deliberately small bootstrapping loop:

```text
host C compiler → TCC → musl + BusyBox + kernel + guest TCC
                     └→ wasm32 TCC → RV64 emulator.wasm → browser terminal
```

The RISC-V64 system is the primary product. i386 remains a second target and
self-hosting check. wasm32 exists to build the browser emulator; it emits a
freestanding final module directly rather than introducing a separate Wasm
object format and linker.

## Compiler

`compiler/` is a reduced TinyCC with separate compilation units for the C
front end, preprocessor, ELF writer, generic assembler, and target backends.
The i386 and RISC-V64 targets produce ELF. The wasm32 target produces the
emulator module. It remains a standard-C compiler: the project does not use a
restricted C dialect to make self-hosting easier.

## Kernel and userspace

`kernel/` is a small standard-C kernel with minimal architecture entry and
context-switch code. It implements processes, Sv39 paging and copy-on-write,
an ELF loader, a writable RAM filesystem, and the syscall surface needed by
static musl, BusyBox `ash`, and TCC.

The kernel always receives an explicit tar initrd. The self-hosting initrd
contains BusyBox, TCC, compiler source, musl headers and libraries, `hello.c`,
and `/selfhost.sh`. Running that script inside the guest builds TCC, then uses
the resulting compiler to build and run the example program.

User mode supplies the Linux ABI expected by musl and BusyBox; it is not a
security boundary. Syscalls trust pointers supplied by guest programs, and all
programs in an initrd belong to one trust domain. Paging and copy-on-write are
process mechanisms here, not protection against malicious guest software.

## Emulator and browser frontend

`emulator/rv64.c` is one C RV64IMA emulator with Sv39, supervisor mode, Sstc,
UART, and the small floating-point subset the guest uses. TCC compiles it to
Wasm for a Web Worker. `emulator/web/` supplies only terminal, fetch, and ELF
loading glue. `emulator/runner.c` is a native terminal frontend for the same
core.

The browser demo is intentionally simple: it is an instruction interpreter,
so guest compilation is much slower than native execution.
