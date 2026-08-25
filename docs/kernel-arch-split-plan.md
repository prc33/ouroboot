# Plan: separating architecture-specific from generic kernel code

Companion to [`kernel-complexity-review.md`](kernel-complexity-review.md)
§12, which established the premise by measurement: most of what looks
riscv64-specific in `kernel/` is generic code in a badly named file.

**Goal.** Every file is unambiguously one of three things — generic,
i386, or riscv64 — with the architecture-specific parts confined to
`arch/i386/` and `arch/riscv64/` behind one small explicit interface.
Membership in a build is then derived from what code actually depends
on, not from which target happened to need it first.

**Non-goal.** This is not a port. It does not make i386 run BusyBox or
self-host. It makes the remaining i386 gap *visible and small* instead
of apparent and large, so that decision can be made on real information.

---

## Premise, restated with evidence

| Claim | Evidence |
|---|---|
| The ramfs is not arch-specific | `mm/ramfs.c` and `mm/tar.c` compile clean for i386 unmodified → valid `ELF 32-bit LSB relocatable, Intel 80386` objects |
| Neither is most of the process layer | 25 of 444 code lines in `sched/riscv64_process.c` touch arch state (~6%) |
| Nor most of the syscall layer | 150 of 828 lines touch `r->aN`, nearly all of it arg marshalling; handler bodies are portable |
| The codebase already knows how to do this | `mm/pmm.c`, `mm/elf.c`, `drivers/kprintf.c` build for both targets with **zero** `#ifdef`s |
| The whole syscall-layer arch difference is 3 operations | number (`eax`/`a7`), args (`ebx…ebp`/`a0`–`a5`), return (`eax`/`a0`) |

---

## Target layout

```
kernel/
  kmain.c                 generic boot flow
  kernel.h                generic kernel-wide declarations
  arch.h                  THE INTERFACE -- every arch_* declaration
  arch/
    i386/                 boot.S gdt.c idt.c isr_stubs.S pic.c pit.c
                          usermode.S syscall_stub.S switch_context.S
                          paging.c trap.c process.c syscall_abi.c
                          syscall_nr.h memmap.h regs.h kend.S
    riscv64/              boot.S entry.c trap_entry.S trap.c timer.c
                          usermode.S switch_context.S
                          paging.c process.c syscall_abi.c
                          syscall_nr.h memmap.h regs.h kend.S
  mm/     pmm.c elf.c ramfs.c tar.c            (generic, both targets)
  sched/  process.c sched.c                    (generic, both targets)
  fs/     syscall_file.c                       (generic file syscalls)
  drivers/ kprintf.c  + arch/*/serial.c
```

The rule that makes this self-enforcing: **nothing outside `arch/*/` may
`#include` anything from inside `arch/*/`, or reference a register by
name.** A file that needs to is, by definition, arch-specific and belongs
in `arch/`. That single rule is checkable mechanically (see Phase 6).

---

## The interface (`kernel/arch.h`)

This is the whole seam. It is deliberately small — if it grows past
roughly this size, something generic is being pushed into `arch/`.

```c
/* --- opaque per-arch types --- */
struct regs;                       /* defined in arch/<arch>/regs.h */
typedef unsigned long arch_addr_t; /* see "address width" below */

/* --- syscall ABI: the entire syscall-layer difference --- */
unsigned long arch_syscall_nr(struct regs *r);
unsigned long arch_syscall_arg(struct regs *r, int n);   /* n = 0..5 */
void          arch_syscall_return(struct regs *r, long value);

/* --- address space --- */
arch_addr_t  *arch_addrspace_new(void);
void          arch_addrspace_activate(arch_addr_t *root);
arch_addr_t  *arch_addrspace_active(void);
void          arch_map_page(arch_addr_t virt, arch_addr_t phys, unsigned flags);
arch_addr_t   arch_get_phys(arch_addr_t virt);
void          arch_fork_cow(arch_addr_t *dst, arch_addr_t *src,
                            arch_addr_t lo, arch_addr_t hi);
void          arch_ensure_writable(arch_addr_t addr, unsigned long len);

/* --- context --- */
void arch_context_init(struct process *p, arch_addr_t entry, arch_addr_t sp);
void arch_context_save(struct process *p);     /* live trapframe -> p */
void arch_context_restore(struct process *p);  /* p -> live trapframe + satp/cr3 */
void arch_context_switch(unsigned long *save_sp, unsigned long new_sp);
void arch_kstack_frame_init(struct process *p, void (*entry)(void));
void arch_exec_rewrite(struct regs *r, arch_addr_t entry, arch_addr_t sp);
void arch_enter_usermode(arch_addr_t entry, arch_addr_t sp) __attribute__((noreturn));

/* --- platform --- */
void          arch_early_init(void);   /* GDT/IDT/PIC/PIT | trap vector/Sstc */
void          arch_halt(void) __attribute__((noreturn));
arch_addr_t   arch_initrd_base(void);  /* fixed addr | Multiboot module */
unsigned long arch_initrd_size(void);
```

Syscall *numbers* stay per-arch (`arch/<arch>/syscall_nr.h`) because the
Linux ABI genuinely differs. Generic dispatch maps a number to a handler
through a table each arch supplies:

```c
struct syscall_entry { unsigned long nr; void (*fn)(struct regs *); };
extern const struct syscall_entry arch_syscall_table[];
```

This also fixes review §10's hand-maintained banner: the printed syscall
list becomes derived from the table rather than a string that has already
drifted out of date.

---

## File-by-file classification

Every current riscv64-build file. "Split" means one file becomes a
generic part plus a small arch part.

### Generic — move as-is, no code change

| Current | Becomes | Lines | Note |
|---|---|---:|---|
| `mm/ramfs.c` / `.h` | `mm/ramfs.c` / `.h` | 376 | **already compiles for i386 (verified)** |
| `mm/tar.c` / `.h` | `mm/tar.c` / `.h` | 117 | **already compiles for i386 (verified)** |
| `mm/pmm.c` / `.h` | unchanged | 197 | already built for both |
| `mm/elf.c` / `.h` | unchanged | 215 | already built for both; runtime ELF32/64 branch |
| `drivers/kprintf.c` | unchanged | 84 | already built for both |

Roughly **990 lines** need no work at all beyond Makefile membership.

### Split — generic body, small arch part

| Current | Generic part | Arch part |
|---|---|---|
| `sched/riscv64_process.c` (444 code lines) | `sched/process.c` — process table, `alloc_slot`, scheduling, `wait4`, fd table, brk/mmap accounting, cwd, drain hook (**~420 lines**) | `arch/<arch>/process.c` — trapframe save/restore, address-space activate, initial context, kstack frame, exec rewrite (**~25 lines each**) |
| `arch/riscv64_syscall.c` (828 code lines) | `fs/syscall_file.c` + `sched/syscall_proc.c` + `mm/syscall_mem.c` — every handler body (**~650 lines**) | `arch/<arch>/syscall_abi.c` — the 3 accessors + `syscall_nr.h` + dispatch table (**~60 lines each**) |
| `sched/process.h` | `sched/process.h` — `struct process`, fd table, API | `arch/<arch>/regs.h` — `struct regs` |
| `mm/paging.h` | `mm/paging.h` — generic PTE flag *names* | `arch/<arch>/paging.h` — flag *values* |
| `riscv64_kmain.c` | `kmain.c` — boot sequence | `arch/<arch>/init.c` |
| `kernel.h` | drop its `#ifndef KERNEL_ARCH_RISCV64` block entirely | declarations move to `arch.h` |

### Arch-specific — move wholesale, rename only

`boot/riscv64_boot.S`, `riscv64_entry.c`, `arch/riscv64_trap_entry.S`,
`arch/riscv64_trap.{c,h}`, `arch/riscv64_timer.c`,
`arch/riscv64_usermode.S`, `arch/riscv64_kend.S`,
`sched/riscv64_switch_context.S`, `mm/riscv64_paging.c`,
`arch/riscv64_memmap.h`, `drivers/riscv64_serial.c` →
`arch/riscv64/*`, dropping the now-redundant `riscv64_` prefix.

i386 equivalents (`boot/boot.S`, `arch/gdt.c`, `arch/idt.{c,h}`,
`arch/isr_stubs.S`, `arch/pic.c`, `arch/pit.c`, `arch/usermode.S`,
`arch/syscall_stub.S`, `arch/kend.S`, `sched/switch_context.S`,
`mm/paging.c`, `drivers/serial.c`) → `arch/i386/*`.

### Delete rather than move

- `sched/riscv64_task.c`, `sched/task.c`, `sched/task.h` — the P4
  fixed-2-task scheduler, superseded by `process_schedule()` (review §1).
  Classifying and moving it would be work spent on code that should not
  survive. `switch_context.S` is **not** in this list — the process
  scheduler still uses it.
- `kernel/hello_elf_payload.h`, `kernel/user_test_payload.h`,
  `kernel/gen_user_test_header.py` — once i386 gets the initrd path
  (Phase 5), 3,754 lines go.

---

## Two decisions to settle before starting

**1. Address width.** `mm/pmm.h` currently uses `unsigned int` for
physical addresses; riscv64 code uses `unsigned long`. This works today
only because riscv64 RAM sits at `0x80000000`–`0x88000000` and fits in 32
bits. It is a latent inconsistency that a split will otherwise cement.
Recommendation: one `arch_addr_t` typedef (`unsigned long` on riscv64,
`unsigned int` on i386), used in every generic signature. Doing this
during Phase 3 is much cheaper than retrofitting later.

**2. Does i386 stay?** If it is going to be archived (review §11 option
1), stop after Phase 1 — the rest of this plan is only worth doing if
both targets live. Phase 1 is worth doing regardless, since it is
essentially free.

---

## Phases

Each phase ends in a committable state with both test suites green:
`make ARCH=i386 test` and `make ARCH=riscv64 test`, plus `test-wasm` and
`test-selfhost` from Phase 3 onward.

### Phase 0 — freeze the oracle
Capture the current passing UART transcripts for i386 and riscv64. Every
later phase is a pure refactor: transcripts must stay byte-identical
except where a phase deliberately changes output. Without this, "still
passes" only means the assertions still match, which is weaker.

### Phase 1 — free win, no code changes
Add `mm/ramfs.o` and `mm/tar.o` to the i386 `OBJS`. They compile
(verified); nothing calls them yet, so behaviour is unchanged. This
proves the premise in the build system itself and makes the "generic
means generic" claim non-theoretical.

**Verify:** both suites unchanged. **Risk:** none.

### Phase 2 — create the layout, move the unambiguous files
Create `arch/i386/` and `arch/riscv64/`; `git mv` every wholesale-arch
file, dropping redundant prefixes. Update includes and `OBJS`. No content
edits — reviewable as pure renames.

**Verify:** both suites green, transcripts identical to Phase 0.
**Risk:** low, but note `arch/riscv64/boot.S` and `trap_entry.S` are raw
`.long`-encoded machine code that hardcodes absolute addresses from
`memmap.h`; moving the *file* is safe, changing any address is not.
Do not touch their contents in this phase.

### Phase 3 — introduce `arch.h`, split the process layer
Define the interface. Split `sched/riscv64_process.c` into generic
`sched/process.c` plus `arch/riscv64/process.c`. Write
`arch/i386/process.c` against the same interface — this is the first
phase that adds real i386 capability, and the first that can genuinely
break something.

Settle the `arch_addr_t` question here.

**Verify:** both suites, plus `test-wasm` and `test-selfhost` — the
self-hosting test is the real regression net for process/exec changes.
**Risk:** highest in the plan. The riscv64 side must come out
byte-identical in behaviour; the i386 side is new code. Recommend doing
riscv64-only first (pure refactor, transcript must not move), then i386
as a separate commit.

### Phase 4 — split the syscall layer
Move handler bodies into generic files; add `arch/<arch>/syscall_abi.c`
and `syscall_nr.h`. Convert dispatch to the table, and derive the startup
banner from it.

**Verify:** as Phase 3. **Risk:** moderate but mechanical — the handler
bodies do not change, only how they read arguments. The banner will
change (it currently omits three implemented syscalls), so Phase 0's
transcript needs that one deliberate exception.

### Phase 5 — i386 initrd, delete the payload headers
Give i386 an initrd via Multiboot modules (`arch_initrd_base()` /
`arch_initrd_size()`). This is genuinely different from riscv64's fixed
`-device loader` address, which is exactly why it is behind the
interface. Then delete `hello_elf_payload.h`, `user_test_payload.h` and
`gen_user_test_header.py`.

**Verify:** i386 suite, updated for initrd-loaded fixtures.
**Removes:** 3,754 lines. **Risk:** moderate, self-contained.

### Phase 6 — enforce the rule
Add a check to the build or CI:

```sh
# no generic file may include from arch/ or name a register
grep -rn '#include "arch/' --include='*.c' --include='*.h' \
     mm/ sched/ fs/ drivers/kprintf.c kmain.c kernel.h
```

Without this, the split will erode the same way the current `OBJS` lists
did. This is what makes the work durable rather than a one-time tidy.

---

## What this does and doesn't buy

**Does:**
- ~990 lines stop being duplicated-in-spirit and become genuinely shared.
- The real i386 gap becomes explicit: a handful of `arch_*` functions,
  not "the filesystem and the process model".
- Adding a third architecture becomes "implement `arch.h`" rather than
  "copy 1,200 lines and rename them" — directly relevant given the
  project already has a wasm32 compiler backend.
- Review §10's drifted syscall banner is fixed structurally.
- Phase 5 removes 3,754 generated lines.

**Doesn't:**
- Make i386 self-host. After all six phases i386 still needs its COW /
  lazy-stack fault path and a BusyBox build to reach riscv64's milestone.
  It makes that remaining work *legible*, which is the point.
- Reduce total line count much on its own (Phase 5 aside) — most of this
  is moving and re-seaming, not deleting. The reductions in
  `kernel-complexity-review.md` §§1–7 are the line-count work; this is
  the structural work. **Do those first** — §1 and §3 in particular
  delete or merge code that would otherwise have to be classified and
  moved here.

## Sequencing against the other review

```
review §1  (boot/test inversion)   ─┐
review §3  (shared image builder)   ├─ do first: shrinks what Phase 3 must move
review §4,5,7 (dedup helpers)      ─┘
        ↓
Phase 1 (free)  →  Phase 2 (moves)  →  Phase 3 (process)  →  Phase 4 (syscall)
        ↓
Phase 5 (i386 initrd)  →  Phase 6 (enforcement)
        ↓
review §11 — decide i386's future, now on accurate information
```

Phase 1 is independent of everything and can be done immediately.
