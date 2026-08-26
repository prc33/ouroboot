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

---

## Status as of 2026-08-26

Done, verified, committed (`c605aa3`, `d997b60`, `34cbf69`):

- `kernel-complexity-review.md` §1/§2 (checkpoint chain moved out of
  the product boot) and §3 (one canonical `build_user_stack()`) — the
  two prerequisites this doc's own "do first" note above asked for.
- The process-layer half of Phase 3: `sched/process.c` (generic) +
  `arch/riscv64_process.c` (the real ~6%: `struct regs`'s own layout,
  CSR/SSTATUS bits, the trap-return mechanism, the hand-built
  kernel-stack-frame convention), talking through seven
  `process_arch_*()` functions declared in `sched/process.h`'s own
  "arch seam" section.

Not done: Phases 1/2 (the free `mm/ramfs.o`/`mm/tar.o` win and the
wholesale directory moves), the syscall-layer half of Phase 3
(`arch/riscv64_syscall.c`'s handler bodies still read `r->aN` directly
— unmixing them needs an `arch_syscall_arg()`-style accessor and a
genuinely large, regression-risky rewrite of ~35 handlers; deferred
rather than rushed), Phases 4-6, and everything below.

## The real remaining piece: i386 self-hosting

This turned out to be substantially larger than "apply the same split
to i386", because i386 doesn't have the *foundation* riscv64's process
layer was built on top of: real per-address-space paging.
`mm/paging.c` today is one fixed page directory, one fixed pool of
page tables (`page_tables[32][1024]`), no COW-across-processes concept
at all — the i386 kernel is still architecturally at the "P5, one
shared address space" stage, not "P6, a real process table". Measured
directly (2026-08-26):

```
mm/paging.h's i386 branch declares 3 functions.
mm/paging.h's riscv64 branch declares 10.
```

Missing: `paging_new_addrspace`, `paging_activate`, `paging_active_root`,
`paging_map_page_in`, `paging_get_phys_in`, `paging_fork_cow`,
`paging_get_flags`, `paging_ensure_writable`. `sched/process.c` calls
five of those unconditionally (by design — see this doc's own note on
why that's the right choice, not an oversight), so it genuinely cannot
link for i386 until they exist, independent of anything else.

### The paging port itself

Same shape as `mm/riscv64_paging.c` (see that file's own extensive
comments — a real, working reference to port *from*, not from
scratch), adapted to i386's 2-level (page directory → page table)
scheme instead of Sv39's 3-level one:

1. **Dynamic table allocation.** The current fixed `page_tables[32][1024]`
   pool assumes one address space. A new address space needs its own
   page directory *and* its own page tables — `alloc_table()` needs to
   come from `pmm_alloc_page()` (already used this way by
   `mm/riscv64_paging.c`'s own `alloc_table()`), not the static pool.
   The pool can stay for the *kernel's own* root table (mirroring
   riscv64's `root_table[]`) or go entirely — either is fine, but the
   per-process tables must be dynamic.
2. **Kernel-sharing granularity — checked, and it's simpler than
   riscv64's was.** i386's kernel identity-maps `[0, 128MB)` at 4MB
   page-directory-entry granularity (`MAX_TABLES=32`, confirmed in
   `mm/paging.c`). The real musl+TCC ELF test binary loads at
   `0x08048198` — **exactly 128MB**, the first address *past* the
   shared region — so sharing all 32 kernel PD entries with every new
   address space, at PD-entry (not finer) granularity, does not
   reproduce riscv64 checkpoint 6's real bug (process addresses
   aliasing onto the same page tables as the kernel or each other):
   the boundary is clean, not scattered. One real constraint this
   creates: **any new per-process test must place its ELF/stack above
   128MB** (`0x08000000`) — the P1-P5 tests' own addresses
   (`kmain.c`'s `USER_TEST_ENTRY = 0x800000`, `user_stack_va =
   0x900000`) are *inside* the shared region and must not be reused
   for a real per-process design; they're fine as-is for what they
   are (single-shared-address-space, pre-checkpoint-6 tests, staying
   unconverged the same way `run_elf_test()` does on the riscv64 side).
3. **`page_fault_handler` gains the COW-copy and lazy-stack-growth
   paths** `mm/riscv64_paging.c`'s already has (`fix_cow_page()` +
   `process_handle_stack_fault()` calls) — i386's current handler only
   has the COW half (`kmain.c`'s own `run_cow_test`), not the stack
   growth half (nothing needs it yet).
4. **CR3, not `satp`** — `paging_activate()` becomes `load_cr3()` plus
   a TLB flush (i386 has no single-instruction `sfence.vma`-equivalent
   for "flush everything"; a full `movl %cr3,%eax; movl %eax,%cr3`
   reload is the standard idiom and already used by `enable_paging()`).

This is real, novel, first-attempt kernel code with no existing i386
implementation to diff against (unlike everything ported *to* riscv64
in this project, which at least had upstream Linux/musl behavior to
check against) — treat it as its own checkpoint, with its own COW
regression test (mirroring `run_cow_test`/`run_cow_user_test`) before
anything downstream depends on it.

### After paging: what's genuinely reusable vs. what's new

**Reusable, close to free**, once `mm/paging.c` implements the same
interface: `sched/process.c` (this doc's own Phase 3 work above) —
zero changes needed, it already only calls `paging_*` by name.
`mm/ramfs.c`/`mm/tar.c` — already confirmed compiling clean for i386
(`kernel-complexity-review.md` §12). Just `arch/i386_process.c` (the
seven `process_arch_*()` functions, i386-flavored: `struct regs`'s own
field names, `eflags`/ring-0-to-3 transition instead of SSTATUS/sret,
`arch/usermode.S`'s existing mechanism for the trap-return equivalent)
needs writing — comparable in size to `arch/riscv64_process.c` (~180
lines).

**Genuinely new, no shortcut:**

- **~20 syscalls**, i386's own `int 0x80` convention (`eax`=number,
  `ebx`/`ecx`/`edx`/`esi`/`edi`/`ebp`=args 1-6, `eax`=return — `arch/syscall.c`
  has exactly 10 today: write, writev, exit, exit_group, brk, mmap2,
  munmap, ioctl, set_thread_area, set_tid_address). Missing, matching
  riscv64's own list: `openat`, `close`, `read`, `execve`, `getcwd`,
  `chdir`, `newfstatat`, `getdents64`, `lseek`, `unlinkat`, `dup3`,
  `clone`, `wait4`, `rt_sigprocmask`, `rt_sigaction`, `sched_yield`,
  `gettid`, `fcntl`, `faccessat`, `getppid`/`geteuid`/`getuid`/`getgid`/`getegid`/`getpid`.
  The handler *logic* for most of these already exists in
  `arch/riscv64_syscall.c` and is arch-neutral in spirit (ramfs lookup,
  dirent formatting, fd table access) — porting means copying that
  logic against i386's own register names, not reinventing it. (This
  is also the concrete case for doing this doc's deferred
  syscall-layer split first, if that work happens before this: it
  would make "port the logic" literally "link the same object".)
- **An initrd mechanism.** riscv64 uses a fixed physical address plus
  QEMU's `-device loader,addr=...`; i386 has no such placement
  primitive at that address on real Multiboot hardware/QEMU's
  Multiboot path. The natural fit is a **Multiboot module** (`grub`/
  QEMU's `-initrd` convention, or a second `-device loader` at a
  fixed address if staying QEMU-specific is acceptable) — genuinely a
  different mechanism, not a different address, which is exactly why
  it's behind `arch_initrd_base()`/`arch_initrd_size()` in this doc's
  original interface sketch rather than assumed to be `RV64_INITRD_BASE`-shaped.
- **An i386 BusyBox build.** Real head start here:
  `demo/build-busybox-i386.sh` and a matching compat patch already
  exist and are referenced as proven in `docs/busybox-findings.md` —
  this is re-running/re-verifying an existing asset, not writing one.
  `musl-i386` is also already built and present in this sandbox.
- **An i386 self-hosting test**, mirroring `test-selfhost` — needs TCC
  built with `TARGET=i386` (already proven: `make TARGET=i386
  selfcheck` passes) linked against `musl-i386`, packaged the same way
  `test/build-selfhost-initrd.sh` does, run through whatever the i386
  initrd mechanism turns out to be.

### Suggested phase order for this piece specifically

1. i386 paging port + its own COW regression test (checkpoint-6
   equivalent) — the one everything else is blocked on.
2. `arch/i386_process.c` + wiring `sched/process.c` into i386's `OBJS` —
   at this point i386 has a real process table and can run the
   existing P1-P5-style tests through it, provable with the *existing*
   `hello`/ring3 fixtures before any new syscall exists.
3. The ~20 syscalls, ported from `arch/riscv64_syscall.c`'s own logic.
4. The Multiboot initrd mechanism + `mm/ramfs.c`/`mm/tar.c` wired into
   i386's `OBJS` (already proven to compile; this is the linking step).
5. i386 BusyBox build re-verified in this sandbox.
6. The i386 self-hosting test.

Each step has its own real regression test and should be committed and
verified independently, the same discipline every riscv64 checkpoint
in this project's history used — this is not a "do it all in one
commit" undertaking, and shouldn't be treated as one.
