# Kernel: P5 checkpoint 1 (ring3 + syscall dispatch) — done

## What exists, on top of P4

```
kernel/
  arch/usermode.S          enter_usermode: hand-built iret frame, ring0 -> ring3
  arch/syscall_stub.S       int $0x80 entry point (separate from the generated stubs --
                             the one gate in the IDT that needs DPL=3)
  arch/syscall.c            Linux i386 ABI syscall dispatch: SYS_write, SYS_exit
  user_test/user_test.S     Hand-written, no-libc ring3 test payload
  gen_user_test_header.py   Assembles/links/extracts user_test.S -> a checked-in C header
  user_test_payload.h       Generated: the payload as a static byte array
```

Extends the same `kernel.elf` / `make test`, now 20 required markers.

## What this checkpoint deliberately does NOT include yet

No ELF loader (the payload is hand-assembled machine code, embedded as
a static array via `user_test_payload.h` -- a stand-in for "load a
program from a filesystem", since there is no ramfs/VFS yet). No real
file descriptors (`SYS_write` just forwards straight to the serial
console regardless of fd, as long as it's 1 or 2). No `musl` runtime
involved. This checkpoint exists to isolate exactly one risk -- does
ring0->ring3->ring0 actually work, end to end -- before adding ELF
parsing and a real libc on top of it as separate, separately-testable
risks. Both are the next checkpoints.

## Two real bugs, both found by testing, both worth remembering

**PDE permission bits.** First run: a page fault at the payload's own
load address, on a *user-mode read* of a page whose PTE clearly said
present+user. The x86 MMU ANDs the permission bits from the page
directory entry and the page table entry -- a page is only truly
user-accessible if *both* say so. `get_table_for()`'s lazy page-table
allocator (written back in P4, exercised only by kernel-only mappings
until now) created every new PDE with `PRESENT | WRITABLE` and no
`USER` bit, because nothing had needed a user-accessible PDE yet. Fixed
by making every lazily-created PDE permissive by default (`PRESENT |
WRITABLE | USER` unconditionally) and relying on the PTE, as everywhere
else in this kernel, for the real per-page restriction -- which is
both the standard approach and simpler than trying to track "will this
table ever hold a user mapping" at creation time.

**`struct regs` field order.** Second run, no more page fault -- but
the syscall dispatcher read back nonsense: syscall number `8388644`
(0x800024), which is exactly the address of the payload's `msg` string
that had been sitting in `%ecx`. `isr_common_stub` does `pusha`, *then*
`push %eax` (holding a saved `%ds`) to preserve the caller's data
segment across the switch to kernel segments. Since that second push
happens after `pusha` and the stack grows down, the saved-`ds` word
ends up at the *lowest* address -- first in memory, not last. The
`struct regs` in `idt.h` had `ds` declared *after* the eight
general-purpose fields, backwards relative to the real layout.

The reason this didn't break anything in P4: the *aggregate* size
was still right (nine words either way), so `err_code`/`eip` -- the
only fields P4 actually read, in the page-fault and unhandled-exception
paths -- landed at the correct offset regardless of how the nine words
in front of them were internally ordered. It took reading an
*individual* general-purpose register (`eax`, `ebx`, `ecx`, `edx` for
the syscall ABI) to expose that every one of those fields was silently
aliased to the wrong physical register the entire time. Worth
remembering: a struct-overlaid-on-a-stack-frame can look completely
correct under partial exercise and still be wrong in a way that only
shows up once you read the field that happens to be misplaced.

## Verified

1. `enter_usermode` builds a correct hand-made `iret` frame; the CPU
   actually drops to CPL=3 (ring3 code executes, using the ring3 CS/SS
   selectors).
2. `int $0x80` from ring3 is *permitted* (DPL=3 gate) and correctly
   transitions back to ring0, loading `esp0`/`ss0` from the TSS (set
   via `tss_set_kernel_stack`) rather than continuing on the tiny
   4KB user stack.
3. `SYS_write` reads the right registers (after the `struct regs` fix)
   and the payload's string reaches the serial console byte-correct.
4. `SYS_exit` reads the right exit code and terminates cleanly.
5. The full round trip -- ring0 to ring3, syscall back to ring0, and
   a second syscall (`exit`) -- all work without corrupting any kernel
   state (the P4 scheduler's own conclusion routine calls straight into
   this checkpoint on the same, still-valid kernel stack).

## Next

Real ELF loading (parse a program header table, map `PT_LOAD` segments,
set up a proper user stack with argv/envp per the System V i386 ABI),
then the actual payoff: a real musl-linked static binary -- the exact
scenario D4 promises ("the same binary runs on Linux and on us") --
running under this kernel instead of a hand-assembled stand-in. That
will need a wider syscall table (`brk`/`mmap` at minimum for musl's
own startup path) derived the same empirical way as the rest of this
project: `qemu-i386-static -strace` against the real target binary
first, rather than guessing.
