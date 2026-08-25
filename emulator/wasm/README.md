# TCC WebAssembly example

This is the first, deliberately standalone use of Ouroboot's `wasm32`
TCC target. It compiles ordinary freestanding C functions into a WebAssembly
module and calls them from a small browser page.

```sh
make test
make serve
```

Then open <http://localhost:8000>. The module has no WASI or JavaScript
imports. Its non-static C functions and linear memory are exported.

The backend currently targets small freestanding code. It supports scalar
integer and floating-point operations, calls, memory, and ordinary structured
control flow. It does not provide a libc, operating-system interface, dynamic
linking, imported functions, function pointers, inline assembly, or a
WebAssembly object-file format. Calls between functions in the same C input
are emitted directly.

Wasm32 keeps 32-bit pointers but uses native Wasm `i64` locals for C `long
long`. The generic compiler exposes this separately from pointer width: i386
continues to use paired 32-bit words, while riscv64 and wasm32 declare native
64-bit registers.

The initial backend is adapted from the LGPL-2.1-licensed WebAssembly work in
[Blosc/MiniCC](https://github.com/Blosc/minicc), then integrated with this
repository's older, stripped TCC tree. Keeping that provenance explicit is
important while the implementation is reduced around Ouroboot's needs.
