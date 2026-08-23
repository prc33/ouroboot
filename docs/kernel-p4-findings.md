# Kernel: P4 (memory + scheduling) — done

## What exists, on top of P3

```
kernel/
  arch/gdt.c              Flat GDT (code/data) + TSS descriptor
  arch/idt.h, idt.c        IDT setup, exception/IRQ dispatch tables
  arch/gen_isr_stubs.py    Generates arch/isr_stubs.S (see below)
  arch/isr_stubs.S         48 interrupt entry stubs (32 exceptions + 16 IRQs) -- generated
  arch/pic.c               8259 PIC remap (IRQ0-15 -> vectors 32-47) + EOI/mask helpers
  arch/pit.c               8254 timer, periodic IRQ0 at a configurable Hz
  arch/kend.S              Linker-symbol-of-last-resort (see below)
  mm/pmm.h, pmm.c           Bitmap physical page allocator
  mm/paging.h, paging.c     Page directory/tables, identity map, COW page-fault handler
  sched/task.h, task.c      Task struct, init, cooperative round-robin yield
  sched/switch_context.S    The actual stack-swap context switch
```

`make test` boots the kernel and asserts on 14 required markers spanning
every subsystem below, with 3 forbidden strings (`FATAL`, `UNHANDLED
EXCEPTION`, `PAGE FAULT`) that would indicate any of them silently broke.

## Two real bugs found and fixed by testing, not by inspection

**`.macro`/`.rept` aren't in TCC's assembler directive table** (checked
directly against `tcctok.h`'s `DEF_ASMDIR` list before writing any code
that would depend on them). 48 near-identical interrupt stubs is exactly
the kind of boilerplate a macro would normally generate. Rather than
hand-write 48 stubs or gamble on an unverified TCC assembler feature,
`arch/gen_isr_stubs.py` generates the literal `.S` text once, and the
*generated file* is what's checked in and compiled — what TCC actually
sees is plain, unambiguous assembly with no loops or macro expansion of
its own to get wrong. Same philosophy as precomputing the Multiboot
checksum in P3: don't depend on assembler cleverness that hasn't been
verified, when precomputing the result is just as easy and removes the
whole question.

**`CR0.WP` — the COW test failed on the first real run, with a very
informative failure.** `parent_phys == child_phys == shared_phys` and
every one of `parent`, `child`, and the *original* physical page all
read back `2` — meaning both writes landed on the same physical page,
and the original was mutated in place. No page fault happened at all.

This is a well-known x86 gotcha, rediscovered the hard way: the CPU only
enforces a PTE's writable bit against *supervisor-mode* (ring 0) writes
if `CR0.WP` (bit 16) is set. It defaults to 0. Since this entire kernel
runs in ring 0 (no userspace yet — that's P5+), every one of our
"read-only" COW pages was, in practice, fully writable from the
kernel's own code the whole time; the CPU silently let every write
through instead of faulting. One line (`orl $0x80010000` instead of
`$0x80000000` when enabling paging) fixed it. Documented inline in
`paging.c` at the exact spot, since this is the kind of bug that will
look like "the fault handler is broken" long before anyone suspects a
missing CPU control bit.

## `arch/kend.S` — a small, deliberate fragility worth naming

TCC's linker doesn't emit an `_end` symbol automatically (checked directly:
absent from `kernel.elf`'s symbol table in P3). The physical allocator
needs to know where the kernel image actually ends so it doesn't hand
out pages that are still part of the kernel itself. `kend.S` supplies a
zero-size `.bss` label for exactly that — but it's only correct because
TCC's linker (no section GC, no reordering, confirmed during the TCC
strip-down) concatenates sections in strict link-file order, and `kend.o`
is listed last in the Makefile's `OBJS`. This is fine as a decision, but
it's a linking-order invariant with no compiler-enforced guard rail — if
a future object file is appended after it in the link line, the symbol
silently becomes wrong instead of failing to build. Worth a real linker
script once TCC's linker-script support (or a hand-rolled replacement)
exists; noted here rather than silently relied on.

## The scheduler: cooperative yield, timer-paced — and why not forced

The plan's P4 exit criterion is "two tasks alternating on timer IRQ."
What's implemented: `g_ticks` is incremented from the real timer ISR
(genuinely hardware-driven), and each task checks it at a single, safe
point at the top of its own loop, yielding via an explicit
`switch_context` call once enough ticks have passed — not switched out
forcibly from inside the ISR at an arbitrary instruction.

This was a deliberate choice, not a shortcut taken by accident:
`serial_putc`/`kprintf` have no locking. A genuinely async switch
(ripping control away from a task mid-`kprintf` call and resuming a
*different* task that also calls `kprintf`) can interleave two tasks'
output byte-by-byte on the wire. That's not just untidy — it would make
the test harness's string-matching assertions unreliable in a way that's
very hard to distinguish from a real scheduler bug. Coupling the switch
point to "provably not inside a print call" was the cheapest way to get
a *solid*, repeatably-green checkpoint rather than a flaky one.

The mechanism being tested -- building a fake initial stack frame so a
never-before-run task's first `switch_context` lands directly in its
entry function via `ret`, and real save/restore of the four callee-saved
registers plus `esp` for every subsequent switch -- is the genuine
article and is exactly what a forced switch would also use; only the
*trigger point* is cooperative rather than asynchronous. Fully-async
forced preemption (interrupting a task at any instruction, including
mid-`kprintf`) is a real and worthwhile follow-up, but it wants its own
prerequisite first: either a lock around serial output, or routing
kernel prints through a buffer that's flushed at safe points rather than
written byte-by-byte inline. Noted as a follow-up rather than silently
worked around.

## Verified, in order

1. GDT loads, TSS descriptor present (not yet exercised — no ring3).
2. IDT loads; a deliberate `int $3` reaches a registered handler and
   resumes normally via `iret` (proves the full stub → dispatch →
   handler → resume path before trusting it for page faults).
3. PIC remap; IRQ0-15 land on vectors 32-47 with no collision against
   the CPU exception range.
4. PIT fires IRQ0 at 100Hz; `g_ticks` advances.
5. Physical allocator: correct free-page count derived from Multiboot's
   `mem_upper`, minus the kernel's own footprint via `kernel_end`.
6. Paging: identity map covers all usable RAM; a real page fault (from
   the COW test) is caught, handled, and resumed correctly.
7. COW: one physical frame, two virtual aliases, first write to each
   independently triggers copy-allocate-remap; final state is two
   *different* physical pages with independent values, and the original
   frame is provably untouched.
8. Scheduler: two tasks launched from a hand-built fake stack frame,
   alternate correctly for 6 timer-paced switches, terminate cleanly.

## What's explicitly not done yet

Ring3/userspace, real per-process address spaces (this phase's paging is
one shared identity-mapped space), `fork`/`exec`, and everything P5
onward. The TSS is loaded but not yet exercised — that only matters once
a ring0→ring3 transition exists.
