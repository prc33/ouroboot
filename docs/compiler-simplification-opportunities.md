# Compiler simplification opportunities

This is a proposal, not an implementation plan already applied. The compiler's
required functionality remains: standard C, i386/RISC-V64/Wasm32 output,
self-compilation, musl and BusyBox builds, freestanding kernel/emulator builds,
and the command-line/linking features those workflows need.

## Highest-value next steps

1. **Turn the real builds into a compact compatibility specification.** The
   strongest deletion test is not grep; it is rebuilding musl, BusyBox, both
   kernels, the Wasm emulator, and stage-2 TCC. Put those in one reproducible
   target, plus focused C conformance tests for preprocessing, integer and
   floating conversions, structs/bitfields, varargs, VLAs, atomics, and
   initializers. This makes later removal safer without narrowing C.

2. **Replace remaining upstream configuration scaffolding with three explicit
   target descriptions.** Target selection is still spread between `tcc.h`,
   the driver, ELF definitions, and backend files. A small target descriptor
   for pointer/long-double sizes, ELF machine/class, relocation form, and
   backend callbacks would remove repeated target conditionals while retaining
   all three backends. Do this only where it reduces code; avoid introducing a
   generic abstraction larger than the switches it replaces.

3. **Separate generic ELF construction from target relocation policy.**
   `tccelf.c` mixes archive/object reading, symbol resolution, executable
   layout, dynamic linking, and target relocation decisions. Keep the generic
   ELF machinery in one place, but make each supported backend own one compact
   relocation table or switch. This should reduce duplicated target tests and
   make it obvious which relocation types are actually supported.

4. **Audit the integrated assemblers by input corpus, not option usage.** i386
   needs assembly for the kernel, musl, and runtime helpers; RISC-V needs its
   deliberately small directive/intrinsic path. Capture every `.S` file those
   builds consume, then remove only parser/opcode forms outside that corpus.
   Preserve inline assembly as a C feature where the supported targets expose
   it. This area can shrink substantially, but is riskier than dead-target
   removal because build systems generate assembly dynamically.

5. **Simplify the Wasm backend around its actual ABI contract.** The Wasm
   backend is split across `wasm-gen.c`, `wasm-link.c`, `wasm-backend.h`, and
   the relatively large `tccwasm.c`. Document the minimal emitted-module ABI
   first (memory, globals, exports, calls, data and relocations), then look for
   duplicated section buffering and encoding helpers. Prefer one straight
   module-emission pipeline over a miniature ELF-like linker model.

## Decisions worth evaluating separately

- **Dynamic linking:** retain it for now as requested. A future static-only
  decision could remove a meaningful part of `tccelf.c`, PLT/GOT handling, and
  `-shared`/`-soname`/`-rdynamic`, but it changes compiler functionality and
  deserves an explicit product decision plus tests—not an unused-branch pass.
- **Bounds checking/backtraces:** also retain them for now. If kept as promised
  functionality, add a build/test configuration that actually enables them;
  otherwise their compile-time-disabled implementation will keep decaying.
- **libtcc API:** the project drives TCC through its CLI, but the API is small
  and closely shares implementation with the driver. Removing it may save less
  than expected while making the compiler less generally useful. Measure the
  reachable delta before deciding.
- **Command-line compatibility options:** do not delete merely because current
  scripts omit them. Options used by common configure/Kbuild probes can be
  valuable even when they are no-ops. Classify each as implemented,
  compatibility no-op, or rejected; silent partial behavior is worse than a
  few table entries.

## Low-risk cleanup after coverage exists

- Remove compile-time developer tracing blocks that have no supported build
  mode, or expose one consistent tracing mechanism instead of many macros.
- Collapse `ONE_SOURCE`/separate-compilation remnants if the repository will
  permanently use the unity build.
- Remove stale compatibility comments and declarations left after deleted
  subsystems; several currently describe history rather than contracts.
- Generate repetitive token/relocation tables from small declarative lists
  only when the generated form is checked in or trivially reproducible. Avoid
  adding a heavyweight generator dependency to save modest C code.
- Review `elf.h` (currently about 3,300 lines) against the exact ELF constants
  required by the three backends and supported dynamic linker. This is likely
  a large low-risk data-definition reduction, provided dynamic-link tests
  establish the required set first.

## What not to simplify

- Do not replace standard C with a project-specific subset.
- Do not special-case the compiler solely for its own source; it must continue
  compiling musl, BusyBox, kernel code, and ordinary examples.
- Do not merge i386's paired-word representation back into the 64-bit
  backends. Native 64-bit values are the simpler model for RISC-V64 and Wasm;
  the unavoidable 32-bit lowering belongs in i386.
- Do not trade source size for opaque generated binary tables or checked-in
  generated payloads. A simplification should remain readable and rebuildable.

The next practical move should therefore be coverage first, followed by the
target-description and ELF-relocation cleanup. Those improve the architecture
without predetermining the separate dynamic-linking, bounds-checking, libtcc,
or CLI compatibility decisions.
