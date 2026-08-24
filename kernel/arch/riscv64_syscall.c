/* Syscall dispatch, Linux riscv64 ABI: a7=number, a0-a5=args 1-6,
 * return value in a0. Same "derived from a real strace, not guessed"
 * methodology as arch/syscall.c -- these are exactly the syscalls
 * `qemu-riscv64-static -strace` showed our own musl-linked riscv64
 * hello binary (from the compiler-port work) actually calling on its
 * way to main() and back: set_tid_address, brk, mmap, munmap, ioctl,
 * writev, exit_group. See docs/riscv-port-findings.md. sched_yield
 * (checkpoint 6), clone/wait4/rt_sigprocmask (checkpoint 7) were added
 * the same way, later -- each syscall's own comment below says which
 * real binary and real strace it came from.
 *
 * Notably shorter than i386's list: riscv64 needs no
 * set_thread_area -- TLS is just the `tp` register, set directly by
 * musl's own _start (arch/riscv64/crt_arch.h), no syscall involved.
 * mmap (222) replaces i386's separate mmap2 -- riscv64 only has the
 * one, offset in bytes not pages (irrelevant here, anonymous-only).
 *
 * brk_current/next_mmap_addr below are still the single "one ring3
 * context at a time" file-static globals i386's own arch/syscall.c
 * uses -- checkpoint 6/7's sched/riscv64_process.c gives every
 * process its own address space and kernel stack, but *not* yet its
 * own brk/mmap state, so two real processes both calling malloc()
 * would corrupt each other's heap bookkeeping. Not yet a problem in
 * practice: every checkpoint 6/7 test payload (sched/riscv64_process.c's
 * own comments explain why) deliberately avoids malloc/printf for
 * exactly this reason. Needs fixing before any real multi-process
 * binary that mallocs runs concurrently with another. */
#include "kernel.h"
#include "riscv64_trap.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/ramfs.h"
#include "sched/process.h"

#define SYS_ioctl             29
#define SYS_sched_yield      124
#define SYS_write              64
#define SYS_writev              66
#define SYS_exit                  93
#define SYS_exit_group              94
#define SYS_set_tid_address           96
#define SYS_brk                         214
#define SYS_munmap                        215
#define SYS_mmap                            222
#define SYS_clone                            220
#define SYS_wait4                              260
#define SYS_rt_sigprocmask                        135
#define SYS_gettid                                    178
#define SYS_openat                                        56
#define SYS_close                                            57
#define SYS_read                                                63
#define SYS_execve                                                221

#define AT_FDCWD (-100)

#define MAP_FIXED  0x10
#define MAP_ANON   0x20

#define PROT_NONE  0x0

#define EBADF   9
#define EINVAL  22
#define ENOTTY  25
#define ENOMEM  12
#define ENOSYS  38
#define ENOENT   2

/* --- process address-space bookkeeping (see file comment) --- */
#define BRK_BASE   0x40000000UL
#define MMAP_BASE  0x60000000UL

static unsigned long brk_current = 0;
static unsigned long next_mmap_addr = MMAP_BASE;

static unsigned long page_round_up(unsigned long x) {
	return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1UL);
}

static void sys_write_impl(unsigned long fd, const char *buf, unsigned long count) {
	if (fd == 1 || fd == 2) {
		for (unsigned long i = 0; i < count; i++)
			serial_putc(buf[i]);
	}
}

static void sys_write(struct regs *r) {
	unsigned long fd = r->a0;
	if (fd != 1 && fd != 2) {
		r->a0 = (unsigned long)-EBADF;
		return;
	}
	sys_write_impl(fd, (const char *)r->a1, r->a2);
	r->a0 = r->a2;
}

/* struct iovec { void *iov_base; size_t iov_len; } -- 16 bytes on riscv64 */
static void sys_writev(struct regs *r) {
	unsigned long fd = r->a0;
	unsigned long *iov = (unsigned long *)r->a1;
	unsigned long iovcnt = r->a2;
	if (fd != 1 && fd != 2) {
		r->a0 = (unsigned long)-EBADF;
		return;
	}
	unsigned long total = 0;
	for (unsigned long i = 0; i < iovcnt; i++) {
		const char *base = (const char *)iov[i * 2];
		unsigned long len = iov[i * 2 + 1];
		sys_write_impl(fd, base, len);
		total += len;
	}
	r->a0 = total;
}

static void sys_exit_impl(struct regs *r, const char *which) {
	kprintf("\n[process exited with code %ld]\n", (long)r->a0);
	kprintf("ring3 test OK\n");
	kprintf("%s\n", which);
}

static void sys_exit(struct regs *r) {
	sys_exit_impl(r, "P5 checkpoint 1 OK");
	run_elf_test();
}

/* checkpoint 6: once sched/riscv64_process.c's process_run() has
 * started (process_mode_active()), exit_group means "this one process
 * is done", not "halt everything" -- process_exit_current() reschedules
 * to whatever else is still runnable, or halts only once nothing is
 * left. Before that point (every P4/P5 checkpoint), exit_group keeps
 * its original one-shot meaning unchanged: conclude P5 checkpoint 2
 * and hand off to run_process_test() (riscv64_kmain.c) instead of
 * halting outright -- this is the one call site that used to be the
 * kernel's final halt and is now where checkpoint 6 actually starts. */
static void sys_exit_group(struct regs *r) {
	if (process_mode_active())
		process_exit_current((int)r->a0); /* noreturn */
	sys_exit_impl(r, "P5 checkpoint 2 OK");
	run_process_test();
}

static void sys_sched_yield(struct regs *r) {
	process_schedule();
	r->a0 = 0;
}

/* checkpoint 7: real fork(), via clone() -- riscv64 has no separate
 * SYS_fork; confirmed via a real `qemu-riscv64-static -strace` of our
 * own musl-linked fork()+wait4() test binary that musl's fork() itself
 * calls exactly `clone(SIGCHLD, NULL, NULL, NULL, NULL)` (musl's own
 * src/process/fork.c, for any arch with no dedicated fork syscall).
 * Only that one specific call shape is implemented -- see
 * sched/process.h's process_fork() comment for what it actually does.
 * Real clone() (CLONE_VM/CLONE_THREAD real threading, a caller-
 * supplied child stack) isn't -- ENOSYS is the honest answer for
 * anything else, not a silently wrong one. */
#define CLONE_FORK_FLAGS 0x11UL /* SIGCHLD (17), no other flags */

static void sys_clone(struct regs *r) {
	unsigned long flags = r->a0;
	unsigned long child_stack = r->a1;
	if (flags != CLONE_FORK_FLAGS || child_stack != 0) {
		r->a0 = (unsigned long)-ENOSYS;
		return;
	}
	int pid = process_fork(r);
	r->a0 = pid < 0 ? (unsigned long)-ENOMEM : (unsigned long)pid;
}

static void sys_wait4(struct regs *r) {
	int pid = (int)(long)r->a0;
	int *status = (int *)r->a1;
	r->a0 = (unsigned long)process_wait4(pid, status);
}

/* honest stub: no real signal delivery/masking exists yet -- just
 * needs to succeed. musl's fork() unconditionally has the child reset
 * its signal mask (confirmed in the same strace as sys_clone's own
 * comment); real semantics don't matter until this kernel has real
 * signal delivery at all. */
static void sys_rt_sigprocmask(struct regs *r) {
	(void)r;
	r->a0 = 0;
}

static void sys_brk(struct regs *r) {
	unsigned long requested = r->a0;

	if (brk_current == 0)
		brk_current = BRK_BASE; /* first call: musl queries with NULL first */

	if (requested == 0) {
		r->a0 = brk_current;
		return;
	}
	if (requested > brk_current) {
		unsigned long old_top = page_round_up(brk_current);
		unsigned long new_top = page_round_up(requested);
		for (unsigned long va = old_top; va < new_top; va += PAGE_SIZE) {
			unsigned long phys = pmm_alloc_page();
			if (!phys) {
				r->a0 = brk_current; /* out of memory: brk unchanged, per Linux semantics */
				return;
			}
			paging_map_page(va, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
		}
	}
	/* shrinking: update accounting only, don't reclaim pages yet --
	 * documented simplification, see file comment */
	brk_current = requested;
	r->a0 = brk_current;
}

static void sys_mmap(struct regs *r) {
	unsigned long addr = r->a0;
	unsigned long length = r->a1;
	/* r->a2 = prot, r->a3 = flags, r->a4 = fd, r->a5 = offset */
	unsigned long prot = r->a2;
	unsigned long flags = r->a3;

	/* Real Linux returns EINVAL for a zero-length mmap; musl's static
	 * TLS setup relies on exactly that (a program needing no TLS
	 * block, like this test binary, computes a genuinely zero-sized
	 * request and expects it to fail so its fallback path can skip
	 * the reservation). Returning a fake "success" here instead --
	 * which the pre-this-fix code did, falling through to the general
	 * path below where the length-0 mapping loop below correctly maps
	 * *nothing* but still reports success -- handed musl an address
	 * it believed was safely backed and wasn't, which page-faulted on
	 * the first real write. Found by tracing exactly which mmap call
	 * returned the address that later faulted. */
	if (length == 0) {
		r->a0 = (unsigned long)-EINVAL;
		return;
	}

	unsigned long len = page_round_up(length);

	if ((flags & MAP_FIXED) && prot == PROT_NONE) {
		/* musl's TLS/stack guard-page reservation: reserve the range
		 * (never map it, so any real access correctly faults) rather
		 * than actually backing it with memory. */
		r->a0 = addr;
		return;
	}

	unsigned long base;
	if (flags & MAP_FIXED) {
		base = addr;
	} else {
		base = next_mmap_addr;
		next_mmap_addr += len;
	}

	for (unsigned long va = base; va < base + len; va += PAGE_SIZE) {
		unsigned long phys = pmm_alloc_page();
		if (!phys) {
			r->a0 = (unsigned long)-ENOMEM;
			return;
		}
		paging_map_page(va, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
	}
	r->a0 = base;
}

static void sys_munmap(struct regs *r) {
	unsigned long addr = r->a0;
	unsigned long len = page_round_up(r->a1);
	for (unsigned long va = addr; va < addr + len; va += PAGE_SIZE) {
		unsigned long phys = paging_get_phys(va);
		if (phys) {
			paging_map_page(va, 0, 0); /* clear PTE_PRESENT */
			pmm_free_page(phys & ~0xFFFUL);
		}
	}
	r->a0 = 0;
}

#define TIOCGWINSZ 0x5413

static void sys_ioctl(struct regs *r) {
	unsigned long fd = r->a0;
	unsigned long req = r->a1;
	if ((fd == 1 || fd == 2) && req == TIOCGWINSZ) {
		/* honest answer: there's no real tty layer yet (P7), and our
		 * fd 1/2 is a serial line, not a terminal -- ENOTTY is what a
		 * non-tty stdout genuinely returns here on real Linux too. */
		r->a0 = (unsigned long)-ENOTTY;
		return;
	}
	r->a0 = (unsigned long)-EINVAL;
}

/* checkpoint 6/7: real pid (process_current_pid()), not the hardcoded
 * "tid 1" every process used to get back before there was more than
 * one -- see process.h's own comment. Falls back to 1 if there's no
 * process context at all (every P4/P5 one-shot test predates
 * process_run() being called even once). */
static unsigned long current_tid(void) {
	int pid = process_current_pid();
	return pid ? (unsigned long)pid : 1UL;
}

static void sys_set_tid_address(struct regs *r) {
	/* Real semantics (clear this address + futex-wake on thread exit)
	 * don't matter yet -- no threads, no futex. Just needs to succeed
	 * and return a plausible tid. */
	(void)r;
	r->a0 = current_tid();
}

static void sys_gettid(struct regs *r) {
	r->a0 = current_tid();
}

/* checkpoint 8: real open()+read()+close(), against mm/ramfs.h's
 * fixed embedded file table -- see that file's own comment for what
 * "real" means here (actual bytes an actual musl+TCC binary actually
 * reads, just not backed by a real block device or writable
 * namespace yet). fd numbers 3.. (0/1/2 stay the UART console,
 * unaffected) -- sched/process.h's process_fd_alloc/get/close index
 * by the raw fds[] slot, this file applies the +3 offset. */
#define PATH_MAX_LOCAL 64

static int copy_path_from_user(char *dst, const char *user_src) {
	int i = 0;
	while (user_src[i] && i < PATH_MAX_LOCAL - 1) {
		dst[i] = user_src[i];
		i++;
	}
	dst[i] = 0;
	return i;
}

static void sys_openat(struct regs *r) {
	/* a0=dirfd, a1=path, a2=flags, a3=mode -- dirfd is ignored (only
	 * AT_FDCWD/absolute paths make sense with no real cwd concept
	 * yet), flags/mode too (this ramfs is read-only, nothing to
	 * create or truncate). */
	char path[PATH_MAX_LOCAL];
	copy_path_from_user(path, (const char *)r->a1);

	const struct ramfs_file *file = ramfs_lookup(path);
	if (!file) {
		r->a0 = (unsigned long)-ENOENT;
		return;
	}
	int idx = process_fd_alloc();
	if (idx < 0) {
		r->a0 = (unsigned long)-ENOMEM; /* table full -- real errno would be EMFILE/ENFILE, close enough here */
		return;
	}
	struct fd_entry *fd = process_fd_get(idx);
	fd->data = file->data;
	fd->size = file->size;
	fd->pos = 0;
	r->a0 = (unsigned long)(idx + 3);
}

static void sys_close(struct regs *r) {
	unsigned long fd = r->a0;
	if (fd == 0 || fd == 1 || fd == 2) {
		r->a0 = 0; /* no-op close of the console -- honest enough, nothing to release */
		return;
	}
	if (fd < 3 || !process_fd_get((int)fd - 3)) {
		r->a0 = (unsigned long)-EBADF;
		return;
	}
	process_fd_close((int)fd - 3);
	r->a0 = 0;
}

/* checkpoint 8: real blocking stdin read (fd 0) -- spin-yield until a
 * byte is available, same "cooperative block" idea as
 * SYS_sched_yield/SYS_wait4's own spin loops, then a single-byte read.
 * Returning fewer bytes than requested is POSIX-legal (read() has
 * never promised to fill the buffer); one raw byte at a time, no
 * echo/line-editing (no tty/line-discipline layer exists yet -- an
 * honest, documented gap, not a bug: whatever's on the other end of
 * the UART is responsible for its own local echo for now). fd>=3
 * reads from mm/ramfs.h via this process's own fd table instead. */
static void sys_read(struct regs *r) {
	unsigned long fd = r->a0;
	unsigned char *buf = (unsigned char *)r->a1;
	unsigned long count = r->a2;

	if (fd == 0) {
		if (count == 0) {
			r->a0 = 0;
			return;
		}
		while (!serial_rx_ready())
			process_schedule();
		buf[0] = serial_getc();
		r->a0 = 1;
		return;
	}
	if (fd == 1 || fd == 2) {
		r->a0 = (unsigned long)-EBADF; /* stdout/stderr aren't readable */
		return;
	}

	struct fd_entry *entry = fd >= 3 ? process_fd_get((int)fd - 3) : 0;
	if (!entry) {
		r->a0 = (unsigned long)-EBADF;
		return;
	}
	unsigned long remaining = entry->size - entry->pos;
	unsigned long n = count < remaining ? count : remaining;
	for (unsigned long i = 0; i < n; i++)
		buf[i] = entry->data[entry->pos + i];
	entry->pos += n;
	r->a0 = n;
}

/* checkpoint 8: real execve() -- sched/process.h's process_execve()
 * does the actual work (new address space, new stack, rewrites the
 * live trapframe in place); this just copies argv/envp's user-space
 * pointer arrays into kernel memory first (each element is itself a
 * user pointer, walked here rather than inside process_execve() so
 * that function's own signature can just be "two NUL-terminated
 * char* arrays", arch-neutral in spirit even though nothing else
 * uses it yet). */
#define EXECVE_MAX_ARGV 8

static void sys_execve(struct regs *r) {
	char path[PATH_MAX_LOCAL];
	copy_path_from_user(path, (const char *)r->a0);

	char *argv[EXECVE_MAX_ARGV + 1];
	unsigned long *user_argv = (unsigned long *)r->a1;
	int argc = 0;
	if (user_argv)
		while (argc < EXECVE_MAX_ARGV && user_argv[argc]) {
			argv[argc] = (char *)user_argv[argc];
			argc++;
		}
	argv[argc] = 0;

	int ret = process_execve(r, path, argv, 0);
	if (ret < 0)
		r->a0 = (unsigned long)-ENOENT;
	/* on success, process_execve() already rewrote every register
	 * that matters -- r->a0 is meaningless here either way (execve()
	 * doesn't "return" 0 on success, it just doesn't return to this
	 * call site at all) */
}

static void syscall_dispatch(struct regs *r) {
	switch (r->a7) {
	case SYS_write:            sys_write(r); return;
	case SYS_writev:           sys_writev(r); return;
	case SYS_exit:              sys_exit(r); return;
	case SYS_exit_group:        sys_exit_group(r); return;
	case SYS_brk:                sys_brk(r); return;
	case SYS_mmap:                 sys_mmap(r); return;
	case SYS_munmap:                 sys_munmap(r); return;
	case SYS_ioctl:                    sys_ioctl(r); return;
	case SYS_sched_yield:                 sys_sched_yield(r); return;
	case SYS_set_tid_address:            sys_set_tid_address(r); return;
	case SYS_clone:                         sys_clone(r); return;
	case SYS_wait4:                            sys_wait4(r); return;
	case SYS_rt_sigprocmask:                      sys_rt_sigprocmask(r); return;
	case SYS_gettid:                                 sys_gettid(r); return;
	case SYS_openat:                                    sys_openat(r); return;
	case SYS_close:                                        sys_close(r); return;
	case SYS_read:                                            sys_read(r); return;
	case SYS_execve:                                              sys_execve(r); return;
	default:
		kprintf("FATAL: unimplemented syscall %lu\n", r->a7);
		r->a0 = (unsigned long)-ENOSYS;
	}
}

void syscall_init(void) {
	syscall_set_handler(syscall_dispatch);
	kprintf("syscall: dispatch installed (write, writev, exit, exit_group, "
		"brk, mmap, munmap, ioctl, sched_yield, set_tid_address, "
		"clone, wait4, rt_sigprocmask, gettid, openat, close, read, execve)\n");
}
