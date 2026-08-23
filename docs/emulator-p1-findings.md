# Emulator P1+P2: headless boot, full kernel test parity

Both `docs/emulator-plan.md` milestones reached in the same session:
P1 (boot `kernel.elf` under Node, reach the first checkpoint) and P2
(every checkpoint `make ARCH=riscv64 test` checks, not just boot) --
`make ARCH=riscv64 test-js` (`emulator/js/boot.js` underneath) passes
the exact same 20 `--must-contain` / 4 `--must-not-contain` assertions
`make ARCH=riscv64 test` (QEMU) does, against the exact same
`kernel.elf`, sharing one assertion list in `kernel/Makefile` so the
two can't silently drift apart.

## What's implemented

`emulator/js/`: `memory.js` (flat RAM window + MMIO dispatch),
`uart.js` (16550, matching `drivers/riscv64_serial.c`'s register
expectations), `csr.js` (named CSR addresses), `mmu.js` (Sv39 walker,
matching `mm/riscv64_paging.c`'s exact PTE layout), `cpu.js` (RV64IM +
Zicsr + the privileged subset the kernel uses -- no M-mode at all, see
`docs/emulator-plan.md`'s E4), `elf.js` (ELF64 loader, independent
from `mm/elf.c`'s in-kernel one since this is what loads the kernel
itself), `boot.js` (Node CLI, mirrors `kernel/test/boot_test.py`'s
flag names on purpose).

## The one real bug, found by booting and tracing (not guessed)

`transmit_empty()` always read 0 from the UART's LSR register, so
`serial_putc`'s poll loop (`while (!transmit_empty());`) spun forever
-- looked exactly like a hang: no crash, no output, instruction count
climbing with `cpu.pc` cycling through the same few addresses.

Root cause: `Memory.read()`/`write()` pass the MMIO offset to a
device's `read`/`write` as a raw `BigInt` (since every *address* in
this emulator is a `BigInt` -- RV64 is a 64-bit ISA, see `memory.js`'s
own header comment for why that's a correctness-first choice here).
But `Uart16550.read()`/`write()` switch on that offset against plain
`Number` case labels (`REG_LSR = 5`, etc). `5n === 5` is `false` in
JavaScript -- BigInt/Number strict equality never matches even when
numerically equal -- so every `case` silently missed and fell through
to the default (`return 0`), for *every* device register access, not
just LSR. serial_init's own writes (IER/LCR/DLL/DLH/FCR/MCR) all
"succeeded" from the CPU's perspective (writes don't have a return
value to get wrong), so nothing looked broken until the first *read*
that mattered.

Found by: per-instruction PC tracing (`cpu.step()` in a loop, printing
`cpu.pc` each iteration) once a fixed instruction budget produced no
output -- narrowed to `mmio_read8` being reached repeatedly, then to
the specific register values at the `beqz` checking
`transmit_empty()`'s result, then to testing `Memory.read()` against
the UART device directly in isolation (bypassing the CPU entirely),
which reproduced it in three lines. The same kind of "isolate to the
smallest repro, don't guess" debugging this whole project has used
throughout the compiler and kernel ports.

Fix: `Number(addr - dev.base)` at the point offsets cross from the
BigInt-addressed memory bus into a device's Number-offset register
space -- device registers are always small, so this loses no
precision, and is the natural place for that conversion to happen
(the boundary between "real 64-bit address space" and "a device's own
small register file"), not something every device needs to handle
itself.

## Performance, honestly

Correctness-first BigInt-everywhere interpreter, zero optimization
attempted yet (deliberately -- see `docs/emulator-plan.md`'s E2/E4
ordering: get it *right* before making it fast). ~1M instructions/sec
on this session's host. The full P2 suite (`pmm_init`'s two ~32K-page
bitmap loops, `paging_init`'s per-page Sv39 walk over 128MB, musl's
real startup + `malloc`/`printf`) is ~11M instructions, ~20-25s
wall-clock with `--time-advance 100` (speeds up the virtual Sstc
clock relative to instructions retired, so the scheduler test's
`wfi`-spin-wait doesn't need tens of millions of idle steps to see 10
ticks -- a test-harness tuning knob, not a CPU-correctness concern).
Worth knowing if this ever needs to run in CI or interactively in a
browser tab; not a blocker for P1/P2's own exit criteria, which say
nothing about speed.

## Still open (see docs/emulator-plan.md for the fuller phase list)

- P3: `xterm.js` + Web Worker, actual browser tab instead of Node.
- P4: F/D (float/double) decode/execute -- confirmed needed for real
  userspace binaries beyond the kernel's own scope (the self-hosted
  `compiler/stage1/tcc` binary uses real hardware `fadd.d`/`fld`/etc),
  not yet implemented at all (Milestone A in the plan deliberately
  scoped this out).
- No differential testing against QEMU yet (register/memory lockstep,
  the way the *original* i386 emulator plan intended) -- P2's
  assertion-string-level parity is a real but weaker guarantee than
  that would be. Worth revisiting once real userspace binaries
  (busybox, self-hosted TCC) are in scope and a subtler divergence
  would be easier to miss with string-matching alone.
