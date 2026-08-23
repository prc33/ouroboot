# Kernel: P5 checkpoint 2 (real ELF loader, real musl binary) — done

This is the checkpoint the plan's D4 decision was written for: "the
same static musl binary runs on Linux and on us." It now does, byte
for byte, unmodified.

## What exists, on top of P5 checkpoint 1

```
kernel/
  mm/elf.h, elf.c          ELF32 PT_LOAD loader
  user_test/hello.elf       The exact musl+tcc binary from the original
                             compiler spike, embedded unmodified
  arch/syscall.c             expanded: write, writev, exit, exit_group,
                             brk, mmap2, munmap, ioctl, set_thread_area,
                             set_tid_address
  arch/gdt.c                  expanded: 9 GDT entries (was 6), slots 6-8
                             reserved for TLS, gdt_set_tls_entry()
  arch/idt.h, gen_isr_stubs.py  struct regs now saves ds/es/fs/gs
                             individually (was one shared field)
```

`make test` now asserts 25 required markers ending in the real
program's actual stdout line (`hello from musl+tcc, sum=285`) and
`P5 checkpoint 2 OK`.

## The syscall list was derived, not guessed

`qemu-i386-static -strace` against our own musl+tcc `hello` binary
(from the original compiler spike) gave the exact, ordered list of
syscalls a real static musl program's startup path makes:

```
set_thread_area(...) = -1 errno=22
modify_ldt(...) = 0
set_tid_address(...) = 464
brk(NULL) = 0x08054000
brk(0x08056000) = 0x08056000
mmap2(0x08054000, 4096, PROT_NONE, MAP_FIXED, ...) = 0x08054000
mmap2(NULL, 4096, PROT_READ|PROT_WRITE, ...) = 0x40814000
munmap(...) = 0
ioctl(1, TIOCGWINSZ, ...) = -1 errno=25
writev(1, ..., 2) = 29
exit_group(0)
```

Every syscall number was cross-checked against musl's own
`arch/i386/bits/syscall.h.in` rather than trusted to memory. Two
things worth noting about the trace itself:

- **`modify_ldt` is qemu-user's fallback, not something this kernel
  needed to implement.** Under real qemu-user, `set_thread_area`
  genuinely fails (that's qemu-user's own emulation limitation, not a
  general Linux one), so musl's own code falls back to the older
  `modify_ldt` mechanism. Since our kernel implements `set_thread_area`
  directly and it's supposed to succeed, `modify_ldt` should never be
  reached from our kernel at all -- and once the real bug below was
  fixed, it wasn't.
- **`writev`, not `write`.** musl's buffered stdio calls `writev`
  under the hood, not the simpler `write` our P5-checkpoint-1
  hand-assembled payload used directly. Both are implemented now.

## Three real bugs, all found by running it

**1. Segment registers collapsed into one field (found by reading
ahead, not by a failing test).** `struct regs` only saved `ds` and
restored that single value into `ds`/`es`/`fs`/`gs` alike on syscall
return. `set_thread_area`'s entire purpose is making `%gs` point at a
*different* selector than the others -- so the very first syscall
after TLS setup would have silently overwritten it back. Caught this
while working out what `set_thread_area` needed to do correctly,
before it could turn into a confusing runtime bug. Fixed by saving all
four segment registers individually (regenerated `isr_stubs.S`); full
P5-checkpoint-1 suite re-run and still 20/20 before building anything
new on top of the change.

**2. Page-directory permission bits (same class of bug as P5 checkpoint
1's, different call site).** A user-mode page fault on a page that was
clearly present. Same root cause as before: x86 ANDs the PDE and PTE
permission bits, and a newly-created page table for the ELF's load
region didn't have `PTE_USER` set on its PDE. Same fix as before
(every lazily-created PDE is permissive by default; the PTE is where
the real restriction lives).

**3. A genuine TCC linker bug, diagnosed properly rather than routed
around blindly.** `set_thread_area` returned `entry_number` (musl's
cached "-1" sentinel, read from a small `.data` constant) as
`0xff000020` instead of `0xFFFFFFFF`. Register dumps confirmed the
*pointer* and the *other* three struct fields (`base_addr`, `limit`,
`flags`) were all correct -- only this one value was wrong, which
ruled out another struct-layout bug and pointed at the data itself.

musl's `__set_thread_area.s` computes the address of that constant
with a classic position-independent-code idiom: `call 1f` to push the
current instruction pointer, then `addl $4f-1b,(%esp)` to add the
compile-time-computed *difference* between two same-section-relative
local labels (one in `.text`, one in `.data`). Confirmed the bug by
hand: compiled and linked a minimal standalone reproduction with our
own tcc, then compared the linker's actual relocated immediate value
against the real address of the `.data` constant (`objdump -s`). The
computed target landed exactly 3 bytes before where the constant
actually lives -- TCC's linker resolves this specific
"difference-of-two-labels-across-a-section-gap" relocation slightly
wrong, most likely related to how it accounts for the page-alignment
padding it inserts between `.text` and `.data`.

This is a real TCC limitation, not something to patch away inside
TCC's linker under time pressure -- that's a bigger, riskier
investigation than this checkpoint warranted. Since this kernel fully
owns the syscall's semantics (there's only ever one TLS user right
now, and it always gets GDT slot 6 regardless of what was requested),
the honest, minimal fix was on the kernel side: stop depending on
`entry_number` being meaningful at all. `base_addr` was always correct
and is all `set_thread_area` actually needs here. Documented at the
exact spot in `arch/syscall.c`, including how the bug was confirmed,
so a future attempt at fixing it in TCC itself doesn't have to
re-derive this.

## What "the same binary" actually means here

`user_test/hello.elf` is a byte-identical copy of the binary produced
during the original musl/TCC compiler spike -- same musl `libc.a`, same
`libtcc1.a`, same static link, same `qemu-i386-static` that ran it
there. It was never recompiled, patched, or relinked for this kernel.
The only new code is *this kernel's* side of the interface (the ELF
loader and the syscall table) -- exactly the D4 promise: a binary that
runs correctly on real Linux (or `qemu-i386-static`, its userspace
stand-in) runs correctly here too, unmodified.

## What's explicitly not done yet

Still only one process ever runs at a time -- no `fork`/`exec`, no
process table, no scheduler integration with ring3 tasks (the P4
scheduler and the P5 ring3 tests are still two separate demonstrations
chained by checkpoint scaffolding in `sys_exit`/`conclude_scheduler_test`,
not real process management). No filesystem -- both the ring3 test
payload and the real ELF are embedded as static data, standing in for
"load a program from disk." Multiple mmap regions, `brk` shrinking that
actually reclaims pages, and non-EXEC (PIE/`ET_DYN`) binaries are all
unimplemented. All reasonable next steps toward P6.
