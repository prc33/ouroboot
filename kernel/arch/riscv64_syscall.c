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
#define SYS_getcwd                                                    17
#define SYS_chdir                                                        49
#define SYS_newfstatat                                                       79
#define SYS_rt_sigaction                                                        134
#define SYS_getppid                                                                 173
#define SYS_geteuid                                                                    175
#define SYS_fcntl                                                                         25
#define SYS_getuid                                                                            174
#define SYS_getgid                                                                               176
#define SYS_getegid                                                                                 177
#define SYS_getpid                                                                           172
#define SYS_getdents64                                                                          61
#define SYS_lseek                                                                                   62
#define SYS_unlinkat                                                                                   35
#define SYS_dup3                                                                                           24
#define SYS_faccessat                                                                                          48

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030

#define AT_FDCWD (-100)

/* checkpoint 12: real values from musl's own headers, same "confirm
 * against the real ABI, don't guess" methodology as every syscall
 * number in this file -- octal in musl's own fcntl.h, not decimal. */
#define O_WRONLY 01
#define O_RDWR   02
#define O_CREAT  0100
#define O_TRUNC  01000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define MAP_FIXED  0x10
#define MAP_ANON   0x20

#define PROT_NONE  0x0

#define EBADF   9
#define EINVAL  22
#define ENOTTY  25
#define ENOMEM  12
#define ENOSYS  38
#define ENOENT   2
#define ENOTDIR 20

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

/* checkpoint 12: shared by sys_write's fd 0/1/2 (only when
 * redirected -- see process.h's own stdio_override comment) and
 * fd>=3 paths -- both end up writing through the same kind of
 * dynamic-file fd_entry, so this is the one real place that does it.
 * Returns 0 on success, -EBADF (a fixed, read-only file -- no
 * O_WRONLY path ever hands one of these back with a real dynfile) or
 * -ENOMEM (pmm_alloc_contiguous() couldn't grow it -- real
 * fragmentation/OOM, not a bug) as a *negative errno*, not a plain
 * -1, so callers can just store it straight into r->a0. */
static long write_to_dynfile(struct fd_entry *entry, const char *buf, unsigned long count) {
	if (!entry->dynfile)
		return -EBADF;
	if (ramfs_dynamic_write(entry->dynfile, entry->pos, (const unsigned char *)buf, count) < 0)
		return -ENOMEM;
	entry->pos += count;
	return 0;
}

static void sys_write(struct regs *r) {
	unsigned long fd = r->a0;
	unsigned long count = r->a2;

	if (fd <= 2) {
		struct fd_entry *ov = process_stdio_get((int)fd);
		if (ov) {
			/* real shell redirection (`echo x > file`) -- ash dup3()'d
			 * a real writable file onto this fd (see sys_dup3's own
			 * comment); write through it exactly like any fd>=3 write. */
			long ret = write_to_dynfile(ov, (const char *)r->a1, count);
			r->a0 = ret < 0 ? (unsigned long)ret : count;
			return;
		}
		if (fd == 0) {
			r->a0 = (unsigned long)-EBADF; /* stdin, not redirected -- not writable */
			return;
		}
		sys_write_impl(fd, (const char *)r->a1, count);
		r->a0 = count;
		return;
	}

	/* checkpoint 12: real file writes -- the actual point of a
	 * writable ramfs. */
	struct fd_entry *entry = process_fd_get((int)fd - 3);
	if (!entry) {
		r->a0 = (unsigned long)-EBADF;
		return;
	}
	long ret = write_to_dynfile(entry, (const char *)r->a1, count);
	r->a0 = ret < 0 ? (unsigned long)ret : count;
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
	int options = (int)r->a2;
	if (status)
		paging_ensure_writable((unsigned long)status, sizeof(int));
	r->a0 = (unsigned long)process_wait4(pid, status, options);
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
	 * yet); mode too (mm/ramfs.c's dynamic files are always 0644-ish
	 * in spirit, real permission bits aren't modeled). flags *are*
	 * honored now -- checkpoint 12, see below. */
	char path[PATH_MAX_LOCAL];
	copy_path_from_user(path, (const char *)r->a1);
	unsigned long flags = r->a2;

	/* checkpoint 11: opendir(".") -- musl's own opendir() (src/dirent/
	 * opendir.c) is just open(name, O_DIRECTORY|...), so this is the
	 * only place left to recognize "you're opening the one directory
	 * this ramfs has", same "." /"/" special case sys_newfstatat
	 * already makes for stat(). Real bug, found running real `ls`:
	 * stat(".") already worked (correctly reporting a directory), so
	 * ash's own cwd-tracking never noticed anything wrong, but open(".")
	 * fell through to ramfs_lookup() -- which only ever matches real
	 * *files* -- and failed with ENOENT ("ls: can't open '.'"). */
	if ((path[0] == '.' && path[1] == 0) || (path[0] == '/' && path[1] == 0)) {
		int idx = process_fd_alloc();
		if (idx < 0) {
			r->a0 = (unsigned long)-ENOMEM;
			return;
		}
		struct fd_entry *fd = process_fd_get(idx);
		fd->data = 0;
		fd->size = 0;
		fd->pos = 0; /* which root-directory entry sys_getdents64 hands out next */
		fd->is_dir = 1;
		fd->dynfile = 0;
		r->a0 = (unsigned long)(idx + 3);
		return;
	}

	/* checkpoint 12: O_CREAT -- a real writable file, dynamically
	 * created (or, with O_TRUNC, freshly emptied) in mm/ramfs.c rather
	 * than looked up in the fixed table. Checked *before* the fixed-
	 * table lookup below: a caller passing O_CREAT is asking for a
	 * file it can write, not whatever fixed (read-only) entry happens
	 * to share its name -- e.g. tcc_write_elf_file's own real
	 * unlink()-then-open(O_CREAT|O_TRUNC|O_WRONLY) pattern (compiler/
	 * tccelf.c) needs a genuinely fresh, writable file every time,
	 * not a silent reopen of something else. */
	if (flags & O_CREAT) {
		struct ramfs_dynamic_file *dyn = ramfs_dynamic_open_or_create(path);
		if (!dyn) {
			r->a0 = (unsigned long)-ENOMEM; /* every dynamic-file slot in use */
			return;
		}
		if (flags & O_TRUNC)
			ramfs_dynamic_truncate(dyn);
		int idx = process_fd_alloc();
		if (idx < 0) {
			r->a0 = (unsigned long)-ENOMEM;
			return;
		}
		struct fd_entry *fd = process_fd_get(idx);
		fd->data = 0;
		fd->size = 0;
		fd->pos = 0;
		fd->is_dir = 0;
		fd->dynfile = dyn;
		r->a0 = (unsigned long)(idx + 3);
		return;
	}

	/* No O_CREAT: must already exist. Dynamic files take priority over
	 * the fixed table -- once something has been written, later opens
	 * (for reading or writing) should see that fresh content, not
	 * silently fall back to a same-named fixed entry. */
	struct ramfs_dynamic_file *dyn = ramfs_dynamic_lookup(path);
	if (dyn) {
		int idx = process_fd_alloc();
		if (idx < 0) {
			r->a0 = (unsigned long)-ENOMEM;
			return;
		}
		struct fd_entry *fd = process_fd_get(idx);
		fd->data = 0;
		fd->size = 0;
		fd->pos = 0;
		fd->is_dir = 0;
		fd->dynfile = dyn;
		r->a0 = (unsigned long)(idx + 3);
		return;
	}

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
	fd->is_dir = 0;
	fd->dynfile = 0;
	r->a0 = (unsigned long)(idx + 3);
}

static void sys_close(struct regs *r) {
	unsigned long fd = r->a0;
	if (fd == 0 || fd == 1 || fd == 2) {
		/* checkpoint 12: if this fd is currently redirected (real
		 * shell I/O redirection -- process.h's own stdio_override
		 * comment), closing it reverts to the plain console; a no-op
		 * otherwise, same as before -- either way "honest enough,
		 * nothing real to release" (this kernel's fd 0/1/2 are always
		 * valid, there's no genuine closed state to model). */
		process_stdio_clear((int)fd);
		r->a0 = 0;
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
/* checkpoint 12: shared by sys_read's redirected-fd-0 and fd>=3
 * paths -- both read from the same kind of fd_entry (a dynamic file's
 * real content lives in its own struct ramfs_dynamic_file, not
 * entry->data/size, which sys_openat leaves at 0/0 for these; a fixed
 * file's lives in entry->data/size directly). Returns the number of
 * bytes actually copied. */
static unsigned long read_from_fd_entry(struct fd_entry *entry, unsigned char *buf, unsigned long count) {
	const unsigned char *src = entry->dynfile ? entry->dynfile->data : entry->data;
	unsigned long src_size = entry->dynfile ? entry->dynfile->size : entry->size;
	unsigned long remaining = src_size - entry->pos;
	unsigned long n = count < remaining ? count : remaining;
	paging_ensure_writable((unsigned long)buf, n);
	for (unsigned long i = 0; i < n; i++)
		buf[i] = src[entry->pos + i];
	entry->pos += n;
	return n;
}

static void sys_read(struct regs *r) {
	unsigned long fd = r->a0;
	unsigned char *buf = (unsigned char *)r->a1;
	unsigned long count = r->a2;

	if (fd == 0) {
		if (count == 0) {
			r->a0 = 0;
			return;
		}
		/* real shell input redirection (`command < file`) -- ash
		 * dup3()'d a real file onto fd 0 (see sys_dup3's own comment);
		 * read through it instead of ever touching the UART. */
		struct fd_entry *ov = process_stdio_get(0);
		if (ov) {
			r->a0 = read_from_fd_entry(ov, buf, count);
			return;
		}
		while (!serial_rx_ready())
			process_schedule();
		paging_ensure_writable((unsigned long)buf, 1); /* see mm/riscv64_paging.c's own comment -- real bug found here first */
		unsigned char c = serial_getc();
		/* ICRNL, by hand: every real tty driver translates an
		 * incoming CR to LF before a line-buffered reader ever sees
		 * it (that's what termios' ICRNL flag is), because a real
		 * terminal always sends '\r' (0x0D) for Enter, never '\n' --
		 * confirmed for this kernel's own case by instrumenting
		 * xterm.js's term.onData in the browser demo: pressing Enter
		 * produces byte 13, not 10. shell/ash.c's own lexer only ever
		 * treats '\n' as end-of-line, so without this translation no
		 * real terminal (this browser demo, or a real one attached to
		 * a real QEMU `-serial stdio` in interactive mode, as opposed
		 * to checkpoint 10's own piped-literal-\n test input) could
		 * ever get a command line to execute at all. There's no tty
		 * layer to put this in otherwise (sys_read's own header
		 * comment), so it lives right here at the only place raw
		 * bytes become a line a shell reads. */
		if (c == '\r')
			c = '\n';
		/* Local echo, by hand, same reasoning as the ICRNL translation
		 * right above: on a real tty, ECHO is the *kernel* tty
		 * driver's job, not the application's -- a shell only does
		 * its own echoing when it's disabled canonical/ECHO mode
		 * itself to do real line-editing (arrow keys, history), which
		 * this busybox build doesn't do (FEATURE_EDITING is off --
		 * see demo/build-busybox-riscv64.sh's allnoconfig-plus-applets
		 * list; shell/ash.c's own preadfd() falls back to a plain
		 * read() with zero echo logic without it). Without a tty layer
		 * to do this for us, sys_read is the only place left, same as
		 * ICRNL -- echo the byte actually stored (post-translation, so
		 * Enter echoes as '\n', which serial_putc() below already
		 * turns into a real "\r\n" for display, same as any other
		 * newline this kernel prints). No backspace/line-editing
		 * support is added here -- that needs a real line discipline
		 * (a whole feature, not a one-line fix); this only restores
		 * the baseline "I can see what I'm typing" a human doing
		 * interactive work expects, without asking them to write their
		 * own line editor first. */
		serial_putc(c);
		buf[0] = c;
		r->a0 = 1;
		return;
	}
	if (fd == 1 || fd == 2) {
		r->a0 = (unsigned long)-EBADF; /* stdout/stderr aren't readable */
		return;
	}

	struct fd_entry *entry = process_fd_get((int)fd - 3);
	if (!entry) {
		r->a0 = (unsigned long)-EBADF;
		return;
	}
	r->a0 = read_from_fd_entry(entry, buf, count);
}

/* checkpoint 12: real seeking -- needed for real file writes, not
 * just reads: compiler/tccelf.c's own ELF writer lays out sections at
 * their real file offsets via repeated lseek()+write() pairs, not
 * strictly-increasing sequential writes (confirmed by reading it, not
 * assumed -- see mm/ramfs.c's own ramfs_dynamic_write() comment on the
 * sparse-file "hole" this implies). Works the same for a read-only
 * fixed-table fd too (entry->dynfile is 0 there, entry->size is
 * already correct), no separate case needed. */
static void sys_lseek(struct regs *r) {
	unsigned long fd = r->a0;
	long offset = (long)r->a1;
	unsigned long whence = r->a2;

	struct fd_entry *entry = fd >= 3 ? process_fd_get((int)fd - 3) : 0;
	if (!entry) {
		r->a0 = (unsigned long)-EBADF;
		return;
	}
	unsigned long size = entry->dynfile ? entry->dynfile->size : entry->size;
	long new_pos;
	switch (whence) {
		case SEEK_SET: new_pos = offset; break;
		case SEEK_CUR: new_pos = (long)entry->pos + offset; break;
		case SEEK_END: new_pos = (long)size + offset; break;
		default: r->a0 = (unsigned long)-EINVAL; return;
	}
	if (new_pos < 0) {
		r->a0 = (unsigned long)-EINVAL;
		return;
	}
	entry->pos = (unsigned long)new_pos;
	r->a0 = entry->pos;
}

/* checkpoint 12: real unlink -- riscv64 has no plain SYS_unlink, only
 * SYS_unlinkat (musl's own src/unistd/unlink.c: unlink(path) is just
 * unlinkat(AT_FDCWD, path, 0) when SYS_unlink isn't defined for this
 * arch, confirmed by reading it). a0=dirfd, a1=path, a2=flags --
 * dirfd/flags ignored, same "no real cwd" reasoning as sys_openat.
 * Best-effort by design (see mm/ramfs.h's own comment on
 * ramfs_dynamic_unlink()): a no-op, not an error, if no dynamic file
 * by that name exists -- exactly what compiler/tccelf.c's own
 * unlink()-before-create pattern needs (it never checks the return
 * value either way). */
static void sys_unlinkat(struct regs *r) {
	char path[PATH_MAX_LOCAL];
	copy_path_from_user(path, (const char *)r->a1);
	ramfs_dynamic_unlink(path);
	r->a0 = 0;
}

/* checkpoint 12: found running real `rm` (busybox coreutils, checking
 * a target actually exists before removing it) -- a0=dirfd, a1=path,
 * a2=mode, a3=flags, all ignored (same "no real cwd/permissions
 * model" reasoning as sys_openat/sys_newfstatat: this ramfs doesn't
 * model real permission bits, so the only thing access() can honestly
 * answer here is "does this path exist at all", which is what every
 * caller in this codebase actually needs it for). Dynamic files take
 * priority over the fixed table, same reasoning as everywhere else. */
static void sys_faccessat(struct regs *r) {
	char path[PATH_MAX_LOCAL];
	copy_path_from_user(path, (const char *)r->a1);
	if ((path[0] == '.' && path[1] == 0) || (path[0] == '/' && path[1] == 0)) {
		r->a0 = 0;
		return;
	}
	if (ramfs_dynamic_lookup(path) || ramfs_lookup(path)) {
		r->a0 = 0;
		return;
	}
	r->a0 = (unsigned long)-ENOENT;
}

/* checkpoint 12: real shell I/O redirection (`echo x > file`) --
 * ash forks, opens the target file (a real fd >= 3), dup3()s it onto
 * fd 1, closes the original, *then* runs the actual command, which
 * just writes to fd 1 as always. Confirmed via musl's own
 * src/unistd/dup2.c: dup2(old,new) is exactly dup3(old,new,0)
 * whenever old != new on an arch with no legacy SYS_dup2, which
 * riscv64 is. See process.h's own stdio_override comment for the
 * real simplification this makes (copies fd state into the target
 * slot rather than making the two fds genuinely share one open-file
 * position -- correct for this dominant open-dup-close pattern). */
static void sys_dup3(struct regs *r) {
	unsigned long oldfd = r->a0;
	unsigned long newfd = r->a1;
	/* r->a2 (flags, O_CLOEXEC) ignored -- no real fd-flags model */

	if (oldfd == newfd) {
		r->a0 = (unsigned long)-EINVAL; /* real dup3's own rule; dup2 never reaches here when old==new (see musl's own dup2.c) */
		return;
	}

	struct fd_entry *src = oldfd <= 2 ? process_stdio_get((int)oldfd) : process_fd_get((int)oldfd - 3);
	if (!src) {
		r->a0 = (unsigned long)-EBADF; /* oldfd is the plain (non-redirected) console, or genuinely invalid -- nothing to duplicate */
		return;
	}

	if (newfd <= 2) {
		process_stdio_set((int)newfd, src);
	} else {
		int idx = (int)newfd - 3;
		if (idx >= MAX_FDS) {
			r->a0 = (unsigned long)-EBADF;
			return;
		}
		process_fd_set(idx, src);
	}
	r->a0 = newfd;
}

/* checkpoint 11: real ls -- musl's readdir() (src/dirent/readdir.c) is
 * a thin wrapper around this one syscall, refilling its own 2KB
 * buffer whenever exhausted. struct linux_dirent64's layout (this is
 * a Linux kernel ABI struct, not the C library's own struct dirent,
 * even though musl's arch/generic/bits/dirent.h happens to define
 * struct dirent with the exact same field order/sizes and just
 * treats a raw getdents64 buffer as an array of them -- confirmed by
 * reading both, not assumed): d_ino (8), d_off (8), d_reclen (2),
 * d_type (1), then a NUL-terminated name. d_off is real Linux's
 * "seek cookie for the next entry" (what a real filesystem's
 * seekdir()/telldir() round-trips through); nothing this kernel runs
 * calls those, so it's left 0 rather than computed. */
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10

static unsigned int fill_dirent64(unsigned char *buf, unsigned long ino, const char *name, unsigned char d_type) {
	unsigned int namelen = 0;
	while (name[namelen])
		namelen++;
	unsigned int reclen = 19 + namelen + 1; /* d_ino+d_off+d_reclen+d_type, then name+NUL */
	reclen = (reclen + 7) & ~7u; /* round up so the *next* record's d_ino stays 8-byte aligned */
	*(unsigned long *)(buf + 0) = ino;
	*(long *)(buf + 8) = 0; /* d_off -- see this function's own comment */
	*(unsigned short *)(buf + 16) = (unsigned short)reclen;
	buf[18] = d_type;
	for (unsigned int i = 0; i < namelen; i++)
		buf[19 + i] = name[i];
	for (unsigned int i = 19 + namelen; i < reclen; i++)
		buf[i] = 0; /* NUL terminator plus any alignment padding */
	return reclen;
}

static void sys_getdents64(struct regs *r) {
	unsigned long fd_num = r->a0;
	unsigned char *buf = (unsigned char *)r->a1;
	unsigned long count = r->a2;

	struct fd_entry *fd = fd_num >= 3 ? process_fd_get((int)fd_num - 3) : 0;
	if (!fd) {
		r->a0 = (unsigned long)-EBADF;
		return;
	}
	if (!fd->is_dir) {
		r->a0 = (unsigned long)-ENOTDIR;
		return;
	}

	/* fd->pos indexes a virtual entry list: ".", "..", then
	 * mm/ramfs.c's own fixed root directory (ramfs_root_entry_*()),
	 * then (checkpoint 12) every dynamic (writable, created-at-
	 * runtime) file -- so something `tcc -o out.elf ...` just wrote
	 * actually shows up in `ls`, not just fixed/embedded content.
	 * Synthesized here rather than stored anywhere, since there are
	 * always exactly two fixed extra entries every real directory
	 * has. */
	unsigned int num_fixed = ramfs_root_entry_count();
	unsigned int total = 2 + num_fixed + ramfs_dynamic_entry_count();
	unsigned long written = 0;
	while (fd->pos < total) {
		const char *name;
		unsigned char d_type;
		if (fd->pos == 0) { name = "."; d_type = DT_DIR; }
		else if (fd->pos == 1) { name = ".."; d_type = DT_DIR; }
		else if (fd->pos - 2 < num_fixed) {
			unsigned int i = (unsigned int)(fd->pos - 2);
			name = ramfs_root_entry_name(i);
			d_type = ramfs_root_entry_is_symlink(i) ? DT_LNK : DT_REG;
		} else {
			unsigned int i = (unsigned int)(fd->pos - 2 - num_fixed);
			name = ramfs_dynamic_entry_name(i);
			d_type = DT_REG;
		}

		unsigned int namelen = 0;
		while (name[namelen])
			namelen++;
		unsigned int reclen = (19 + namelen + 1 + 7) & ~7u;
		if (written + reclen > count)
			break; /* caller's buffer is full -- stop, resume from here next call */

		paging_ensure_writable((unsigned long)(buf + written), reclen);
		written += fill_dirent64(buf + written, fd->pos + 1, name, d_type);
		fd->pos++;
	}
	r->a0 = written; /* 0 once fd->pos reaches total -- musl's readdir() treats that as EOF */
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

/* checkpoint 9: busybox ash's own startup needs all of these -- see
 * arch/riscv64_syscall.c's git history for the real strace this was
 * derived from (a real ash -c/script run under qemu-riscv64-static,
 * same methodology as everything else in this file).
 *
 * struct stat layout confirmed by compiling a small offsetof() probe
 * with our own riscv64 toolchain and running it under
 * qemu-riscv64-static, rather than hand-deriving field offsets from
 * musl's typedefs (nlink_t/blksize_t/etc.'s actual sizes depend on
 * ifdef branches easy to misread): dev_t/ino_t/rdev at byte offsets
 * 0/8/32 (8 bytes each), mode_t/nlink_t/uid_t/gid_t at 16/20/24/28 (4
 * bytes each), an 8-byte pad, then off_t/blksize_t/blkcnt_t at
 * 48/56/64 (8 bytes each), three 16-byte timespecs from byte 72 --
 * 128 bytes total. Only st_mode and st_size are ever read by anything
 * this kernel runs (mm/ramfs.h's ash-facing PATH search checks
 * S_ISREG(st_mode) and nothing else -- confirmed in ash.c's own
 * source, not guessed), so everything else here is just zeroed
 * rather than computed to match. */
#define ST_MODE_OFF 16
#define ST_SIZE_OFF 48
#define S_IFDIR 0040000
#define S_IFREG 0100000

static void fill_stat(unsigned char *sb, unsigned int mode, unsigned long size) {
	for (int i = 0; i < 128; i++)
		sb[i] = 0;
	*(unsigned long *)(sb + 0) = 1;  /* st_dev */
	*(unsigned long *)(sb + 8) = 1;  /* st_ino */
	*(unsigned int *)(sb + ST_MODE_OFF) = mode;
	*(unsigned int *)(sb + 20) = 1;  /* st_nlink */
	*(long *)(sb + ST_SIZE_OFF) = (long)size;
	*(long *)(sb + 56) = 512;        /* st_blksize */
}

static void sys_newfstatat(struct regs *r) {
	/* a0=dirfd, a1=path, a2=statbuf, a3=flags -- dirfd/flags ignored,
	 * same "no real cwd/directory concept" reasoning as sys_openat. */
	char path[PATH_MAX_LOCAL];
	copy_path_from_user(path, (const char *)r->a1);
	unsigned char *sb = (unsigned char *)r->a2;
	paging_ensure_writable((unsigned long)sb, 128); /* sizeof(struct stat) -- see fill_stat()'s own comment for the layout */

	/* "." and "/" both mean the same thing here (mm/ramfs.h has no
	 * real subdirectories) -- ash's own getpwd() logic stats both its
	 * cached cwd string and "." to cross-check they're the same
	 * place; a directory entry satisfies that unconditionally since
	 * there's only ever the one directory. */
	if ((path[0] == '.' && path[1] == 0) || (path[0] == '/' && path[1] == 0)) {
		fill_stat(sb, S_IFDIR | 0755, 0);
		r->a0 = 0;
		return;
	}

	/* checkpoint 12: dynamic files take priority over the fixed table,
	 * same reasoning as sys_openat's own lookup order -- a freshly
	 * written file should stat() as itself, not (if it happens to
	 * share a name) whatever fixed entry existed first. */
	struct ramfs_dynamic_file *dyn = ramfs_dynamic_lookup(path);
	if (dyn) {
		fill_stat(sb, S_IFREG | 0755, dyn->size);
		r->a0 = 0;
		return;
	}

	const struct ramfs_file *file = ramfs_lookup(path);
	if (!file) {
		r->a0 = (unsigned long)-ENOENT;
		return;
	}
	fill_stat(sb, S_IFREG | 0755, file->size);
	r->a0 = 0;
}

static void sys_getcwd(struct regs *r) {
	/* Always "/" -- mm/ramfs.h has no real subdirectories, so it's
	 * also the only correct answer here, not just a placeholder (same
	 * reasoning as sys_chdir below). */
	char *buf = (char *)r->a0;
	unsigned long size = r->a1;
	if (size < 2) {
		r->a0 = (unsigned long)-EINVAL; /* ERANGE would be more precise; not worth a new errno for this */
		return;
	}
	paging_ensure_writable((unsigned long)buf, 2);
	buf[0] = '/';
	buf[1] = 0;
	r->a0 = 2; /* real getcwd(2) returns the length written, including the NUL */
}

static void sys_chdir(struct regs *r) {
	/* No-op success: mm/ramfs.h is flat, so "changing directory" has
	 * nothing to actually change -- every lookup already matches by
	 * basename regardless of any notion of cwd. Rejecting a chdir
	 * into somewhere that isn't "/" would be more correct but nothing
	 * in this checkpoint's tests needs that distinction yet. */
	(void)r;
	r->a0 = 0;
}

/* Honest stub, same spirit as sys_rt_sigprocmask -- no real signal
 * delivery exists yet, so there's nothing to actually install; ash's
 * own startup installs handlers for SIGINT/SIGQUIT/SIGTERM/SIGCHLD
 * unconditionally and needs this to merely succeed, not actually
 * work (real signal delivery -- e.g. Ctrl-C interrupting a running
 * command -- is future scope: this kernel has no interrupt-driven
 * UART RX to even notice a Ctrl-C arrived independent of whatever's
 * currently blocked in read()). */
static void sys_rt_sigaction(struct regs *r) {
	(void)r;
	r->a0 = 0;
}

static void sys_getppid(struct regs *r) {
	r->a0 = (unsigned long)process_current_ppid();
}

static void sys_geteuid(struct regs *r) {
	/* Always root (0) -- this kernel has exactly one trust domain,
	 * same as every other "no real permissions model yet" corner
	 * (e.g. mm/elf.c maps every segment writable regardless of
	 * p_flags). */
	(void)r;
	r->a0 = 0;
}

static void sys_getuid(struct regs *r) {
	/* checkpoint 10: real ash's own interactive startup calls this
	 * (real uid, not effective) to decide the default prompt ("# " for
	 * root, "$ " otherwise) -- same "always root" answer as geteuid. */
	(void)r;
	r->a0 = 0;
}

static void sys_getgid(struct regs *r) {
	/* Same "always root, one trust domain" reasoning as sys_getuid. */
	(void)r;
	r->a0 = 0;
}

static void sys_getegid(struct regs *r) {
	(void)r;
	r->a0 = 0;
}

static void sys_getpid(struct regs *r) {
	r->a0 = (unsigned long)process_current_pid();
}

/* checkpoint 9: ash dup()s its script fd to a fresh slot right after
 * opening it (shell/ash.c: `fd = fcntl(fd, F_DUPFD_CLOEXEC, 10)`),
 * standard shell practice to keep the script's own fd out of the way
 * of whatever low fd numbers the script's commands might use.
 * F_DUPFD/F_DUPFD_CLOEXEC here allocate a fresh mm/ramfs.h fd table
 * slot and copy the entry (data/size/pos) into it -- a real dup()
 * shares the underlying file description (so both fds' positions
 * stay in sync), this makes an independent copy instead; nothing in
 * this checkpoint's tests reads through both the old and new fd
 * concurrently, so the difference doesn't show. The `arg` minimum-fd-
 * number argument (real dup() promises the new fd is >= arg) is
 * ignored -- this fd table is tiny (MAX_FDS) and dense from 3, always
 * comfortably below whatever avoidance threshold a caller asks for.
 * close-on-exec itself is a no-op for the same reason
 * F_GETFD/F_SETFD are: nothing in this kernel enforces it (fork()/
 * execve() already just keep every fd open, see sched/process.h's
 * own comment on process_fork()'s fd copy), so there's no flag to
 * actually store. */
static void sys_fcntl(struct regs *r) {
	unsigned long fd = r->a0;
	unsigned long cmd = r->a1;

	if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
		if (fd < 3) {
			/* dup'ing the console: no real second console fd to hand
			 * back (fd 0/1/2 aren't reference-counted objects here at
			 * all) -- reporting success as the same fd is honest
			 * enough for what this checkpoint exercises. */
			r->a0 = fd;
			return;
		}
		struct fd_entry *old = process_fd_get((int)fd - 3);
		if (!old) {
			r->a0 = (unsigned long)-EBADF;
			return;
		}
		int idx = process_fd_alloc();
		if (idx < 0) {
			r->a0 = (unsigned long)-ENOMEM;
			return;
		}
		struct fd_entry *nf = process_fd_get(idx);
		nf->data = old->data;
		nf->size = old->size;
		nf->pos = old->pos;
		r->a0 = (unsigned long)(idx + 3);
		return;
	}
	if (cmd == F_GETFD || cmd == F_SETFD || cmd == F_SETFL) {
		r->a0 = 0;
		return;
	}
	if (cmd == F_GETFL) {
		r->a0 = 0; /* O_RDONLY -- every fd this kernel hands out is read-only (mm/ramfs.h) */
		return;
	}
	r->a0 = (unsigned long)-ENOSYS;
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
	case SYS_getcwd:                                                 sys_getcwd(r); return;
	case SYS_chdir:                                                     sys_chdir(r); return;
	case SYS_newfstatat:                                                    sys_newfstatat(r); return;
	case SYS_rt_sigaction:                                                          sys_rt_sigaction(r); return;
	case SYS_getppid:                                                                        sys_getppid(r); return;
	case SYS_geteuid:                                                                             sys_geteuid(r); return;
	case SYS_getuid:                                                                                 sys_getuid(r); return;
	case SYS_getgid:                                                                                    sys_getgid(r); return;
	case SYS_getegid:                                                                                      sys_getegid(r); return;
	case SYS_getpid:                                                                                 sys_getpid(r); return;
	case SYS_fcntl:                                                                                     sys_fcntl(r); return;
	case SYS_getdents64:                                                                                   sys_getdents64(r); return;
	case SYS_lseek:                                                                                           sys_lseek(r); return;
	case SYS_unlinkat:                                                                                           sys_unlinkat(r); return;
	case SYS_dup3:                                                                                                  sys_dup3(r); return;
	case SYS_faccessat:                                                                                               sys_faccessat(r); return;
	default:
		kprintf("FATAL: unimplemented syscall %lu\n", r->a7);
		r->a0 = (unsigned long)-ENOSYS;
	}
}

void syscall_init(void) {
	syscall_set_handler(syscall_dispatch);
	kprintf("syscall: dispatch installed (write, writev, exit, exit_group, "
		"brk, mmap, munmap, ioctl, sched_yield, set_tid_address, "
		"clone, wait4, rt_sigprocmask, gettid, openat, close, read, execve, "
		"getcwd, chdir, newfstatat, rt_sigaction, getppid, geteuid, "
		"getpid, fcntl, getdents64, lseek, unlinkat, dup3, faccessat)\n");
}
