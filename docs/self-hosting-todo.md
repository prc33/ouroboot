# In-kernel self-hosting: complete

The handover goal is now a permanent test:

```sh
make -C kernel ARCH=riscv64 test-selfhost
```

The target builds a RISC-V64 stage-1 TCC with this repository's TCC, packs
that compiler, the TCC sources, headers, runtime, and musl development files
into a tar initrd, and boots the normal kernel under QEMU. Inside BusyBox ash:

1. `/tcc` compiles and links the separate compiler units listed in
   `/tcc-stage2.args` into `/tcc-stage2`;
2. stage 2 runs and reports its version;
3. stage 2 compiles and links a fresh program;
4. that program runs and prints `hello from stage2 tcc`.

The exact guest-side demonstration is `/selfhost.sh`; its input program is the
small `/hello.c`. Both are ordinary files in the initrd, so the same sequence
can be run interactively in the browser. The test also requires the kernel's
final P10 checkpoint and rejects fatal or page-fault output.

## Blockers found and fixed

- The larger exec stack was correct, but `fork()` still cloned a hard-coded
  two-page stack range. Each process now records its stack limit; fork clones
  the actual committed range. Exec reserves a 16 MiB downward-growing stack,
  maps only its top two pages, and adds zeroed pages on user faults. This also
  avoids paying for unused stack in small programs. The argv area is large
  enough for the real compiler command.
- `brk` and the next anonymous `mmap` address were global kernel variables.
  A newly exec'd compiler therefore inherited address-allocation bookkeeping
  from an unrelated process and corrupted its heap. They are now process
  state, copied by fork and reset by exec.
- musl writes stdio with `writev`; the syscall only supported the console, so
  TCC appeared to create object files while silently discarding their bytes.
  `writev` now handles ramfs files and redirected standard descriptors.
- musl's allocator uses `mremap`; the kernel now implements the small
  shrink-or-move subset it needs. Fresh `brk`, `mmap`, and `mremap` pages are
  explicitly zeroed.
- RISC-V uses a 128-bit ABI representation for `long double`. The minimal
  runtime helpers implemented their operations using `long double` itself,
  recursively calling themselves until the compiler exhausted its stack.
  They now pack/unpack the ABI representation explicitly and perform the
  project's intended double-precision approximation without recursion.

## Deliberate limits

This remains a compact demonstration kernel, not a general Linux clone.
`mremap` does not grow in place, fork still clones known address windows rather
than a VMA list, and the long-double runtime has double rather than quad
precision. None weakens the closure proof above.

The C/Wasm emulator can boot the same initrd and run stage 1, but rebuilding
the complete compiler requires a large number of interpreted guest
instructions. QEMU is therefore used for the routine full-closure test; the
browser/Node targets retain faster boot, shell, and real-initrd coverage.
