#ifndef SYSCALL_COMMON_H
#define SYSCALL_COMMON_H

/* Opaque here on purpose -- syscall_posix.c (this header's own lower
 * half) never touches a named field of struct regs, only ever a
 * pointer to one, passed straight through to sys_arg()/sys_ret()/
 * process_fork()/process_execve(). The real definition (genuinely
 * different per arch -- idt.h vs riscv64_trap.h) is whatever the
 * including .c file's own arch headers already pulled in first. */
struct regs;

/* Syscall handler logic genuinely shared between i386 and riscv64 --
 * see docs/kernel-arch-split-plan.md. Each arch's own arch/i386/syscall.c
 * / arch/risc/riscv64_syscall.c still owns its own syscall *numbers*
 * (the Linux ABI genuinely differs there) and reads its own registers
 * (eax/ebx/ecx/... vs a0-a7) -- that ABI-marshalling top-and-tail
 * stays in each arch's own file, deliberately. What moved here is the
 * handler bodies underneath it that turned out, once actually
 * compared side by side, to be either byte-for-byte identical
 * already (sys_write's console loop, the TIOCGWINSZ check, munmap)
 * or *should* have been -- see syscall_grow_pages()'s own comment for
 * two real i386 bugs this fixed as a direct consequence of comparing
 * the two implementations for the first time.
 *
 * Everything here takes and returns plain values, never `struct regs`
 * -- so unlike a syscall_arg()-accessor-based design, nothing here
 * needs either arch's register layout at all. `unsigned long`
 * throughout: i386's own 32-bit values narrow into it and back out
 * losslessly (every address this kernel ever hands out already fits
 * in 32 bits on i386 by construction), the same implicit-narrowing
 * pattern mm/pmm.c and mm/elf.c already rely on to build for both
 * ARCHs from one source file. */

/* Rounds `x` up to the next PAGE_SIZE boundary. */
unsigned long syscall_page_round_up(unsigned long x);

/* The actual byte-write loop behind sys_write/sys_writev's fd 1/2
 * case -- both ARCHs' consoles are a plain serial line (no real tty),
 * so "write" has only ever meant "serial_putc every byte". Each
 * arch's own sys_write additionally checks for redirected stdio
 * before falling back to this (riscv64's real ramfs-backed fd
 * redirection; i386 has no such thing yet), which is why this stays
 * a narrow helper rather than the whole syscall. */
void syscall_write_raw(unsigned long fd, const char *buf, unsigned long count);

/* TIOCGWINSZ handling shared by both ARCHs' sys_ioctl: -ENOTTY for a
 * console fd asking for terminal geometry (honest -- neither ARCH has
 * a real tty layer, and a non-tty stdout genuinely returns this on
 * real Linux too), -EINVAL for anything else this kernel doesn't
 * implement. Returns the value to store as the syscall's return
 * register (already negative, ready to cast straight in). */
long syscall_ioctl_check(unsigned long fd, unsigned long req);

/* Maps [old_top, new_top) (both already page-rounded) with fresh,
 * zeroed, PRESENT|WRITABLE|USER pages -- the growth half of
 * sys_brk, and the allocation loop inside sys_mmap. Returns 1 on
 * success, 0 if pmm_alloc_page() runs out partway through (caller's
 * own brk/mmap value is left at whatever it was before this call --
 * real Linux semantics for brk, and the honest answer for mmap since
 * nothing here undoes the pages it *did* manage to map).
 *
 * The explicit zeroing here is not cosmetic: i386's own sys_brk/
 * sys_mmap2 didn't do this before this file existed -- a real,
 * latent bug (freshly brk'd/mmap'd memory could expose whatever a
 * physical page's *previous* owner left in it) that riscv64's own
 * equivalent already had the fix for, found only once the two
 * implementations were compared side by side to build this shared
 * version. Every other pmm_alloc_page() call site in this kernel
 * already zeroes for the same reason (see sched/process.c's own
 * build_user_stack(), mm/riscv64_paging.c's alloc_table()) -- this
 * was the one place on the i386 side that hadn't caught up. */
int syscall_grow_pages(unsigned long old_top, unsigned long new_top);

/* The unmap loop behind sys_munmap: for each mapped page in
 * [addr, addr+len), clears its PTE and frees the physical page. A
 * no-op (not an error) for any page in the range that was never
 * mapped, matching real munmap()'s own "unmapped regions are
 * silently ignored" semantics. */
void syscall_munmap_pages(unsigned long addr, unsigned long len);

/* --- syscall_posix.c (checkpoint 18, docs/kernel-arch-split-plan.md's
 * "genericize rather than write afresh" instruction, taken all the way
 * this time) ---
 *
 * Everything above this point takes and returns plain values, never
 * `struct regs` -- genuinely arch-neutral *logic*, no register access
 * involved at all. Everything below still operates on `struct regs *r`
 * (the live trapframe, needed for a handful of things a plain value
 * can't carry: process_fork()'s own snapshot, process_execve()'s
 * in-place rewrite), but never touches a named field of it -- every
 * argument comes through sys_arg(), every return value goes through
 * sys_ret(), both tiny accessor functions each arch's own syscall.c
 * implements against its own register convention (eax/ebx/ecx/... vs
 * a0-a7). That's what makes it possible to write these handler bodies
 * -- openat/read/execve/fork/wait4/getdents64/... -- exactly once:
 * once arch/i386/syscall.c grew a real process table to actually run
 * these against (checkpoint 17, arch/i386/process.c), comparing it
 * against arch/risc/riscv64_syscall.c's own versions showed the logic
 * itself never depended on *how* an argument got read out of `r`, only
 * on being able to read it at all -- an accessor-based design was
 * ruled out earlier in this same plan (see this header's own older
 * comment above) specifically because most of this logic depended on
 * the process table i386 didn't have yet; now that it does, sharing it
 * outright is correct instead of premature. */

/* n = 0..5, the syscall's own argument N (Linux ABI convention: never
 * more than 6). sys_ret()'s `val` is stored as-is into the syscall
 * return register -- callers pass already-negated errno values the
 * same way every arch-specific syscall.c always has
 * ((unsigned long)-EBADF etc), sys_ret() itself doesn't interpret it. */
unsigned long sys_arg(struct regs *r, int n);
void sys_ret(struct regs *r, unsigned long val);

/* Fires at most once, the next time a bare SYS_exit or a pre-process-
 * mode SYS_exit_group reaches sys_exit()/sys_exit_group() below --
 * i.e. before process_init()/process_mode_active() exist at all. See
 * process.h's own (now slightly stale, this is the real definition)
 * comment on the declaration itself for the full contract -- moved
 * here from arch/risc/riscv64_syscall.c once sys_exit_impl/sys_exit/
 * sys_exit_group themselves moved (the hook storage has to live next
 * to whoever actually calls it). arch/i386/kmain.c's own checkpoint
 * chain (P5 checkpoint 1 -> checkpoint 2) now goes through this same
 * mechanism instead of the hardcoded run_elf_test() call sys_exit used
 * to make directly -- see that file's own comment. */
void syscall_set_pre_process_exit_hook(void (*hook)(void));

void sys_write(struct regs *r);
void sys_writev(struct regs *r);
void sys_exit(struct regs *r);
void sys_exit_group(struct regs *r);
void sys_sched_yield(struct regs *r);
void sys_clone(struct regs *r);
void sys_fork(struct regs *r); /* i386 only -- see syscall_posix.c's own comment */
void sys_wait4(struct regs *r);
void sys_rt_sigprocmask(struct regs *r);
void sys_rt_sigaction(struct regs *r);
void sys_brk(struct regs *r);
void sys_mmap(struct regs *r);  /* i386's own SYS_mmap2 dispatches here too -- see this function's own comment */
void sys_munmap(struct regs *r);
void sys_mremap(struct regs *r);
void sys_ioctl(struct regs *r);
void sys_set_tid_address(struct regs *r);
void sys_gettid(struct regs *r);
void sys_getppid(struct regs *r);
void sys_geteuid(struct regs *r);
void sys_getuid(struct regs *r);
void sys_getgid(struct regs *r);
void sys_getegid(struct regs *r);
void sys_getpid(struct regs *r);
void sys_openat(struct regs *r);
void sys_open(struct regs *r); /* i386 only -- see syscall_posix.c's own comment */
void sys_close(struct regs *r);
void sys_read(struct regs *r);
void sys_readv(struct regs *r);
void sys_lseek(struct regs *r);
void sys_unlinkat(struct regs *r);
void sys_unlink(struct regs *r); /* i386 only -- see syscall_posix.c's own comment */
void sys_faccessat(struct regs *r);
void sys_access(struct regs *r); /* i386 only -- see syscall_posix.c's own comment */
void sys_dup3(struct regs *r);
void sys_dup2(struct regs *r); /* i386 only -- see syscall_posix.c's own comment */
void sys_fcntl(struct regs *r);
void sys_getdents64(struct regs *r);
void sys_execve(struct regs *r);
/* sys_newfstatat/sys_stat/sys_fstat are NOT declared here -- struct
 * stat's real on-the-wire layout genuinely differs between i386 and
 * riscv64 (confirmed by compiling a real offsetof() probe against
 * each arch's own musl headers, not assumed), so each arch's own
 * syscall.c implements its own version, `static` and file-local. See
 * arch/i386/syscall.c's own comment on sys_newfstatat for the details
 * and the measurement. */
void sys_getcwd(struct regs *r);
void sys_chdir(struct regs *r);

#endif
