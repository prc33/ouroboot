# Kernel: P3 (boot + serial) — done

## What exists

```
kernel/
  boot/boot.S       Multiboot v1 header + _start (stack setup, calls kmain)
  drivers/serial.c  16550 UART (COM1), poll-driven, no interrupts yet
  drivers/kprintf.c Freestanding printf: %d %u %x %X %p %s %c %%
  kmain.c           Entry point: init serial, parse multiboot info, banner
  kernel.h
  Makefile          Builds with ../tcc/tcc, nothing else
  test/boot_test.py The test harness (see below)
```

`make test` from `kernel/`: builds the kernel, boots it under QEMU headless
with serial piped to a captured buffer, asserts on the transcript, exits
nonzero on any assertion failure.

## Decisions carried over and confirmed live

- **D8 (no BIOS/real-mode/bootloader):** used the Multiboot v1 spec — a
  12-byte header (magic/flags/checksum) is all a loader needs to drop the
  CPU straight into 32-bit protected mode with a flat address space and
  jump to `_start`. QEMU's `-kernel` flag speaks this natively, so there's
  no bootloader of ours to write or debug. Checksum is a precomputed
  literal (`0xE4524FFC`) rather than an in-assembler expression, to avoid
  depending on unverified constant-folding in TCC's assembler for
  something that's trivial to get exactly right by hand.
- **D8 (serial only):** 16550 UART, ~45 lines, poll-driven (no PIC/IRQ
  wiring yet — that's P4+). `\n` is expanded to `\r\n` at the driver level
  so the transcript is sane in a real terminal later.
- **Self-hosting constraint:** every object file here is compiled and
  linked by `../tcc/tcc` — the stripped fork from the previous phase.
  Zero host-gcc involvement in the kernel itself. `-Wl,-Ttext=0x100000`
  (TCC's image-base flag) was enough to get correct Multiboot layout;
  TCC has no real linker-script support, but none was needed.

## The test harness — the actual point of this phase

A kernel with no display and no attached debugger is otherwise silent
about its own failures. `test/boot_test.py`:

- Boots under QEMU headless (`-display none`, `-serial stdio`) with a
  hard `timeout` wrapper, so a hang can never stall a test run — a
  kernel that halts forever after printing (the normal, correct case
  right now, since there's no shutdown syscall yet) is *expected* to
  hit the timeout; that's not treated as failure by itself.
- Takes `--must-contain` / `--must-not-contain` string lists and fails
  loudly, listing exactly which assertion failed, plus the full captured
  transcript — so a failure is diagnosable from the test output alone,
  not a mystery that needs re-running by hand.
- **Verified in both directions, not just on the happy path.** Built two
  deliberately-broken kernel variants to confirm the harness actually
  catches failure, not just passes everything:
  - A kernel that hangs before calling `serial_init()` at all → harness
    correctly reports "no serial output at all" (this is the
    triple-fault/early-hang case, the one that's hardest to debug blind,
    and now it's the one case the harness calls out by name).
  - A kernel that deliberately prints `FATAL: test forced failure` →
    harness correctly flags the forbidden string and fails.

This pattern — a required-strings/forbidden-strings assertion list per
phase, run through the same harness — is what P4 onward should build on.
Each phase's exit criterion from the plan maps directly onto a
`--must-contain` list:

| Plan phase | Suggested assertion |
|---|---|
| P4 (paging/COW) | print a marker after a deliberate COW fault-and-recover, e.g. `"COW test: parent=1 child=2 OK"` |
| P5 (first userspace) | have the musl `hello` binary's own stdout reach the serial line, e.g. exact stdout bytes as `--must-contain` |
| P6 (multiprocess) | `ls | wc -l` output value asserted exactly |
| P7 (tty/job control) | scripted `^C`/`^Z`/`fg` sequence, assert on resulting process state markers |

The harness already supports `--qemu-arg` passthrough for phases that need
extra devices (e.g. `-netdev`/`-device` once the NIC exists) and `--mem`
for phases sensitive to available memory (paging/allocator tests).

## What's explicitly not done yet

GDT/IDT/TSS setup, physical page allocator, paging — all P4. Right now
we're running on whatever flat unpaged environment the Multiboot loader
leaves us in; there is no memory protection at all. Next phase should
start there, using the same test-harness pattern: boot, run a scripted
memory operation, assert the expected outcome on the serial transcript.
