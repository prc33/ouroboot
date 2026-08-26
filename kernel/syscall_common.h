#ifndef SYSCALL_COMMON_H
#define SYSCALL_COMMON_H

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

#endif
