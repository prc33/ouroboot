/* Syscall handler bodies shared between i386 and riscv64 -- see
 * syscall_common.h's own "syscall_posix.c" comment for the full
 * rationale. Every argument comes through sys_arg(r, n), every return
 * value through sys_ret(r, val) -- both implemented per-arch (a plain
 * switch over eax/ebx/ecx/.../ebp for i386, a0-a5 for riscv64), never
 * a named `struct regs` field touched directly here. `r` itself is
 * still threaded through to process_fork()/process_execve(), which
 * already treat it opaquely (sched/process.h's own arch seam).
 *
 * This file used to be (most of) arch/risc/riscv64_syscall.c -- every
 * comment below explaining *why* some particular real bug or real
 * strace shaped a given handler is inherited unchanged from there, not
 * rewritten, since none of that reasoning was actually riscv64-
 * specific to begin with. */
#include "kernel.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/ramfs.h"
#include "sched/process.h"
#include "syscall_common.h"

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030

#define AT_FDCWD (-100)

/* checkpoint 12: real values from musl's own headers, same "confirm
 * against the real ABI, don't guess" methodology as every syscall
 * number in each arch's own syscall.c -- octal in musl's own fcntl.h,
 * not decimal. */
#define O_CREAT  0100
#define O_TRUNC  01000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define MAP_FIXED  0x10
#define MREMAP_MAYMOVE 1

#define PROT_NONE  0x0

#define EBADF   9
#define EINVAL  22
#define ENOMEM  12
#define ENOSYS  38
#define ENOENT   2
#define ENOTDIR 20

/* --- process address-space bookkeeping ---
 * Falls back to plain file-static globals before process_init() has
 * ever run (every P4/P5-equivalent one-shot ring3/ELF-loader test on
 * either arch) -- see get_brk_current()'s own comment. */
#define BRK_BASE   0x40000000UL
#define MMAP_BASE  0x60000000UL

static unsigned long brk_current = 0;
static unsigned long next_mmap_addr = MMAP_BASE;

static unsigned long get_brk_current(void) {
	return process_mode_active() ? process_current_brk() : brk_current;
}

static void set_brk_current(unsigned long value) {
	if (process_mode_active())
		process_set_current_brk(value);
	else
		brk_current = value;
}

static unsigned long take_mmap_addr(unsigned long length) {
	if (process_mode_active())
		return process_take_mmap(length);
	unsigned long base = next_mmap_addr;
	next_mmap_addr += length;
	return base;
}

/* checkpoint 12: shared by sys_write's fd 0/1/2 (only when
 * redirected -- see process.h's own stdio_override comment) and
 * fd>=3 paths -- both end up writing through the same kind of
 * dynamic-file fd_entry, so this is the one real place that does it.
 * Returns 0 on success, -EBADF (a fixed, read-only file -- no
 * O_WRONLY path ever hands one of these back with a real dynfile) or
 * -ENOMEM (pmm_alloc_contiguous() couldn't grow it -- real
 * fragmentation/OOM, not a bug) as a *negative errno*, not a plain
 * -1, so callers can just sys_ret() it straight through. */
static long write_to_dynfile(struct fd_entry *entry, const char *buf, unsigned long count) {
	if (!entry->dynfile)
		return -EBADF;
	if (ramfs_dynamic_write(entry->dynfile, entry->pos, (const unsigned char *)buf, count) < 0)
		return -ENOMEM;
	entry->pos += count;
	return 0;
}

void sys_write(struct regs *r) {
	unsigned long fd = sys_arg(r, 0);
	unsigned long count = sys_arg(r, 2);

	if (fd <= 2) {
		struct fd_entry *ov = process_stdio_get((int)fd);
		if (ov) {
			/* real shell redirection (`echo x > file`) -- ash dup3()'d
			 * a real writable file onto this fd (see sys_dup3's own
			 * comment); write through it exactly like any fd>=3 write. */
			long ret = write_to_dynfile(ov, (const char *)sys_arg(r, 1), count);
			sys_ret(r, ret < 0 ? (unsigned long)ret : count);
			return;
		}
		if (fd == 0) {
			sys_ret(r, (unsigned long)-EBADF); /* stdin, not redirected -- not writable */
			return;
		}
		syscall_write_raw(fd, (const char *)sys_arg(r, 1), count);
		sys_ret(r, count);
		return;
	}

	/* checkpoint 12: real file writes -- the actual point of a
	 * writable ramfs. */
	struct fd_entry *entry = process_fd_get((int)fd - 3);
	if (!entry) {
		sys_ret(r, (unsigned long)-EBADF);
		return;
	}
	long ret = write_to_dynfile(entry, (const char *)sys_arg(r, 1), count);
	sys_ret(r, ret < 0 ? (unsigned long)ret : count);
}

/* struct iovec { void *iov_base; size_t iov_len; } -- one native word
 * per field on both arches (4 bytes i386, 8 bytes riscv64), so
 * `unsigned long iov[]` indexing at 2*i/2*i+1 is correct either way. */
void sys_writev(struct regs *r) {
	unsigned long fd = sys_arg(r, 0);
	unsigned long *iov = (unsigned long *)sys_arg(r, 1);
	unsigned long iovcnt = sys_arg(r, 2);
	struct fd_entry *entry = 0;
	if (fd <= 2)
		entry = process_stdio_get((int)fd);
	else
		entry = process_fd_get((int)fd - 3);
	if (fd == 0 || (fd > 2 && !entry)) {
		sys_ret(r, (unsigned long)-EBADF);
		return;
	}
	unsigned long total = 0;
	for (unsigned long i = 0; i < iovcnt; i++) {
		const char *base = (const char *)iov[i * 2];
		unsigned long len = iov[i * 2 + 1];
		if (entry) {
			long ret = write_to_dynfile(entry, base, len);
			if (ret < 0) {
				sys_ret(r, (unsigned long)ret);
				return;
			}
		} else {
			syscall_write_raw(fd, base, len);
		}
		total += len;
	}
	sys_ret(r, total);
}

/* checkpoint 14: fires at most once, before process_init() has ever
 * been called (bare SYS_exit, or SYS_exit_group before there's a
 * process table -- see sys_exit_group's own comment). The product
 * boot on either arch never reaches this at all: it calls
 * process_init() before any process can exit. Each arch's own
 * checkpoint/test chain (kernel/test/riscv64_checkpoints.c;
 * arch/i386/kmain.c's own P4/P5 sequence) registers one to chain into
 * (and print the closing message for) whichever checkpoint comes next
 * -- see process.h's own comment on syscall_set_pre_process_exit_hook(). */
static void (*pre_process_exit_hook)(void) = 0;

void syscall_set_pre_process_exit_hook(void (*hook)(void)) {
	pre_process_exit_hook = hook;
}

static void sys_exit_impl(struct regs *r) {
	kprintf("\n[process exited with code %ld]\n", (long)(long)sys_arg(r, 0));
	if (pre_process_exit_hook) {
		void (*hook)(void) = pre_process_exit_hook;
		pre_process_exit_hook = 0;
		hook();
		return; /* hooks don't return in practice, but this isn't noreturn */
	}
	kprintf("halting.\n");
	arch_halt_forever();
}

void sys_exit(struct regs *r) {
	sys_exit_impl(r);
}

/* checkpoint 6: once sched/process.c's process_run() has started
 * (process_mode_active()), exit_group means "this one process is
 * done", not "halt everything" -- process_exit_current() reschedules
 * to whatever else is still runnable, or halts only once nothing is
 * left (sched/process.c's own process_halt()). Before that point,
 * exit_group falls through to the same pre-process-exit-hook path
 * bare SYS_exit uses above -- see sys_exit_impl's own comment. */
void sys_exit_group(struct regs *r) {
	if (process_mode_active())
		process_exit_current((int)sys_arg(r, 0)); /* noreturn */
	sys_exit_impl(r);
}

void sys_sched_yield(struct regs *r) {
	process_schedule();
	sys_ret(r, 0);
}

/* checkpoint 7: real fork(), via clone() -- neither arch has a
 * separate SYS_fork in this kernel's Linux ABI; confirmed via a real
 * `qemu-*-static -strace` of a real musl-linked fork()+wait4() test
 * binary that musl's fork() itself calls exactly
 * `clone(SIGCHLD, NULL, NULL, NULL, NULL)` (musl's own
 * src/process/fork.c, for any arch with no dedicated fork syscall).
 * Only that one specific call shape is implemented -- see
 * sched/process.h's process_fork() comment for what it actually does.
 * Real clone() (CLONE_VM/CLONE_THREAD real threading, a caller-
 * supplied child stack) isn't -- ENOSYS is the honest answer for
 * anything else, not a silently wrong one. */
#define CLONE_FORK_FLAGS 0x11UL /* SIGCHLD (17), no other flags */

void sys_clone(struct regs *r) {
	unsigned long flags = sys_arg(r, 0);
	unsigned long child_stack = sys_arg(r, 1);
	if (flags != CLONE_FORK_FLAGS || child_stack != 0) {
		sys_ret(r, (unsigned long)-ENOSYS);
		return;
	}
	int pid = process_fork(r);
	sys_ret(r, pid < 0 ? (unsigned long)-ENOMEM : (unsigned long)pid);
}

/* i386 only (arch/i386/syscall.c's dispatch) -- real Linux i386 keeps
 * the legacy zero-argument SYS_fork (2) alongside SYS_clone, and
 * musl-i386's own src/process/_Fork.c prefers it whenever SYS_fork is
 * defined for the target arch (confirmed in musl-i386's own generated
 * bits/syscall.h), which it is for i386 but isn't for riscv64 --
 * see sys_clone's own comment. No flags/child_stack arguments to
 * validate at all (a bare fork() always means exactly
 * CLONE_FORK_FLAGS with no child stack -- that's the *only* thing a
 * zero-argument syscall could mean), so this skips sys_clone's own
 * check entirely rather than reading arguments that were never
 * pushed. */
void sys_fork(struct regs *r) {
	int pid = process_fork(r);
	sys_ret(r, pid < 0 ? (unsigned long)-ENOMEM : (unsigned long)pid);
}

void sys_wait4(struct regs *r) {
	int pid = (int)(long)sys_arg(r, 0);
	int *status = (int *)sys_arg(r, 1);
	int options = (int)sys_arg(r, 2);
	if (status)
		paging_ensure_writable((unsigned long)status, sizeof(int));
	sys_ret(r, (unsigned long)process_wait4(pid, status, options));
}

/* honest stub: no real signal delivery/masking exists yet -- just
 * needs to succeed. musl's fork() unconditionally has the child reset
 * its signal mask (confirmed in the same strace as sys_clone's own
 * comment); real semantics don't matter until this kernel has real
 * signal delivery at all. */
void sys_rt_sigprocmask(struct regs *r) {
	sys_ret(r, 0);
}

void sys_brk(struct regs *r) {
	unsigned long requested = sys_arg(r, 0);
	unsigned long current = get_brk_current();

	if (current == 0)
		current = BRK_BASE;

	if (requested == 0) {
		sys_ret(r, current);
		return;
	}
	if (requested > current) {
		unsigned long old_top = syscall_page_round_up(current);
		unsigned long new_top = syscall_page_round_up(requested);
		if (!syscall_grow_pages(old_top, new_top)) {
			sys_ret(r, current); /* out of memory: brk unchanged, per Linux semantics */
			return;
		}
	}
	/* shrinking: update accounting only, don't reclaim pages yet --
	 * documented simplification, see this file's own header comment */
	set_brk_current(requested);
	sys_ret(r, requested);
}

/* i386's own SYS_mmap2 dispatches here too (arch/i386/syscall.c) --
 * mmap2's extra argument (pgoffset, arg 5, in *pages* not bytes) is
 * simply never read here, same as it never was back when i386 had its
 * own separate sys_mmap2 body: every real caller in this kernel only
 * ever does anonymous mmap (MAP_ANON), where offset is meaningless by
 * definition. */
void sys_mmap(struct regs *r) {
	unsigned long addr = sys_arg(r, 0);
	unsigned long length = sys_arg(r, 1);
	/* arg 2 = prot, arg 3 = flags, arg 4 = fd, arg 5 = offset */
	unsigned long prot = sys_arg(r, 2);
	unsigned long flags = sys_arg(r, 3);

	/* Real Linux returns EINVAL for a zero-length mmap; musl's static
	 * TLS setup relies on exactly that (a program needing no TLS
	 * block computes a genuinely zero-sized request and expects it to
	 * fail so its fallback path can skip the reservation). Real bug
	 * this guards against, found by tracing exactly which mmap call
	 * returned the address that later faulted: returning a fake
	 * "success" instead handed musl an address it believed was safely
	 * backed and wasn't. */
	if (length == 0) {
		sys_ret(r, (unsigned long)-EINVAL);
		return;
	}

	unsigned long len = syscall_page_round_up(length);

	if ((flags & MAP_FIXED) && prot == PROT_NONE) {
		/* musl's TLS/stack guard-page reservation: reserve the range
		 * (never map it, so any real access correctly faults) rather
		 * than actually backing it with memory. */
		sys_ret(r, addr);
		return;
	}

	unsigned long base;
	if (flags & MAP_FIXED) {
		base = addr;
	} else {
		base = take_mmap_addr(len);
	}

	if (!syscall_grow_pages(base, base + len)) {
		sys_ret(r, (unsigned long)-ENOMEM);
		return;
	}
	sys_ret(r, base);
}

void sys_munmap(struct regs *r) {
	unsigned long addr = sys_arg(r, 0);
	unsigned long len = syscall_page_round_up(sys_arg(r, 1));
	syscall_munmap_pages(addr, len);
	sys_ret(r, 0);
}

void sys_mremap(struct regs *r) {
	unsigned long old_addr = sys_arg(r, 0);
	unsigned long old_len = syscall_page_round_up(sys_arg(r, 1));
	unsigned long new_len = syscall_page_round_up(sys_arg(r, 2));
	unsigned long flags = sys_arg(r, 3);
	if (!new_len) {
		sys_ret(r, (unsigned long)-EINVAL);
		return;
	}
	if (new_len <= old_len) {
		syscall_munmap_pages(old_addr + new_len, old_len - new_len);
		sys_ret(r, old_addr);
		return;
	}
	if (!(flags & MREMAP_MAYMOVE)) {
		sys_ret(r, (unsigned long)-ENOMEM);
		return;
	}
	unsigned long base = take_mmap_addr(new_len);
	if (!syscall_grow_pages(base, base + new_len)) {
		sys_ret(r, (unsigned long)-ENOMEM);
		return;
	}
	for (unsigned long i = 0; i < old_len; i++)
		*(unsigned char *)(base + i) = *(unsigned char *)(old_addr + i);
	syscall_munmap_pages(old_addr, old_len);
	sys_ret(r, base);
}

void sys_ioctl(struct regs *r) {
	sys_ret(r, (unsigned long)syscall_ioctl_check(sys_arg(r, 0), sys_arg(r, 1)));
}

/* checkpoint 6/7: real pid (process_current_pid()), not a hardcoded
 * "tid 1" -- see process.h's own comment. Falls back to 1 if there's
 * no process context at all (every one-shot P4/P5-equivalent test on
 * either arch predates process_run() being called even once). */
static unsigned long current_tid(void) {
	int pid = process_current_pid();
	return pid ? (unsigned long)pid : 1UL;
}

void sys_set_tid_address(struct regs *r) {
	/* Real semantics (clear this address + futex-wake on thread exit)
	 * don't matter yet -- no threads, no futex. Just needs to succeed
	 * and return a plausible tid. */
	sys_ret(r, current_tid());
}

void sys_gettid(struct regs *r) {
	sys_ret(r, current_tid());
}

void sys_getppid(struct regs *r) {
	sys_ret(r, (unsigned long)process_current_ppid());
}

/* Always root (0) -- this kernel has exactly one trust domain, same as
 * every other "no real permissions model yet" corner (e.g. mm/elf.c
 * maps every segment writable regardless of p_flags). */
void sys_geteuid(struct regs *r) {
	sys_ret(r, 0);
}

/* checkpoint 10: real ash's own interactive startup calls this (real
 * uid, not effective) to decide the default prompt ("# " for root, "$
 * " otherwise) -- same "always root" answer as geteuid. */
void sys_getuid(struct regs *r) {
	sys_ret(r, 0);
}

void sys_getgid(struct regs *r) {
	sys_ret(r, 0);
}

void sys_getegid(struct regs *r) {
	sys_ret(r, 0);
}

void sys_getpid(struct regs *r) {
	sys_ret(r, (unsigned long)process_current_pid());
}

/* Real open()+read()+close() against the initrd-populated ramfs. fd
 * numbers 3.. (0/1/2 stay the UART console, unaffected) --
 * sched/process.h's process_fd_alloc/get/close index by the raw fds[]
 * slot, this file applies the +3 offset. */
/* checkpoint 14: bumped from 64 -- the tar-loaded initrd's real
 * musl-header/TCC-source paths (mm/ramfs.h's now-full-path-matched
 * dynamic files) top out in the 30s ("musl/arch/riscv64/bits/alltypes.h.in"),
 * so 128 is real headroom, not a tight fit driven by a specific path. */
#define PATH_MAX_LOCAL 128

static int copy_path_from_user(char *dst, const char *user_src) {
	int i = 0;
	while (user_src[i] && i < PATH_MAX_LOCAL - 1) {
		dst[i] = user_src[i];
		i++;
	}
	dst[i] = 0;
	return i;
}

/* Canonicalize absolute or cwd-relative paths into the ramfs key form: no
 * leading slash, with '.' and '..' resolved. */
static void resolve_path(char *out, const char *input, const char *base) {
	unsigned int n = 0, i = 0;
	if (input[0] != '/') {
		if (base[0] == '/') base++;
		while (base[n] && n < PATH_MAX_LOCAL - 1) { out[n] = base[n]; n++; }
	}
	while (input[i]) {
		while (input[i] == '/') i++;
		unsigned int start = i;
		while (input[i] && input[i] != '/') i++;
		unsigned int len = i - start;
		if (!len || (len == 1 && input[start] == '.')) continue;
		if (len == 2 && input[start] == '.' && input[start + 1] == '.') {
			while (n && out[n - 1] != '/') n--;
			if (n) n--;
			continue;
		}
		if (n && n < PATH_MAX_LOCAL - 1) out[n++] = '/';
		for (unsigned int j = 0; j < len && n < PATH_MAX_LOCAL - 1; j++) out[n++] = input[start + j];
	}
	out[n] = 0;
}

static void resolve_user_path(char *out, const char *user_path) {
	char input[PATH_MAX_LOCAL];
	copy_path_from_user(input, user_path);
	resolve_path(out, input, process_current_cwd());
}

/* Shared core of sys_openat() and (i386 only) sys_open() -- see that
 * function's own comment for why i386 needs a separate entry point at
 * all: its legacy SYS_open has no dirfd argument, not just a
 * hardcoded AT_FDCWD one, so the argument *count*, not just the
 * value, genuinely differs from sys_openat's own ABI. */
static void openat_core(struct regs *r, long dirfd, const char *user_path, unsigned long flags) {
	/* Relative paths use cwd or the supplied directory fd; permission
	 * bits are not modeled. */
	char input[PATH_MAX_LOCAL], path[PATH_MAX_LOCAL];
	copy_path_from_user(input, user_path);
	const char *base = process_current_cwd();
	if (input[0] != '/' && dirfd != AT_FDCWD) {
		struct fd_entry *df = dirfd >= 3 ? process_fd_get((int)dirfd - 3) : 0;
		if (!df || !df->is_dir) { sys_ret(r, (unsigned long)-EBADF); return; }
		base = df->path;
	}
	resolve_path(path, input, base);

	/* Directories are inferred from stored file path prefixes. */
	if (ramfs_is_dir(path)) {
		int idx = process_fd_alloc();
		if (idx < 0) {
			sys_ret(r, (unsigned long)-ENOMEM);
			return;
		}
		struct fd_entry *fd = process_fd_get(idx);
		fd->data = 0;
		fd->size = 0;
		fd->pos = 0;
		fd->is_dir = 1;
		fd->dynfile = 0;
		copy_path_from_user(fd->path, path);
		sys_ret(r, (unsigned long)(idx + 3));
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
			sys_ret(r, (unsigned long)-ENOMEM); /* every dynamic-file slot in use */
			return;
		}
		if (flags & O_TRUNC)
			ramfs_dynamic_truncate(dyn);
		int idx = process_fd_alloc();
		if (idx < 0) {
			sys_ret(r, (unsigned long)-ENOMEM);
			return;
		}
		struct fd_entry *fd = process_fd_get(idx);
		fd->data = 0;
		fd->size = 0;
		fd->pos = 0;
		fd->is_dir = 0;
		fd->dynfile = dyn;
		sys_ret(r, (unsigned long)(idx + 3));
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
			sys_ret(r, (unsigned long)-ENOMEM);
			return;
		}
		struct fd_entry *fd = process_fd_get(idx);
		fd->data = 0;
		fd->size = 0;
		fd->pos = 0;
		fd->is_dir = 0;
		fd->dynfile = dyn;
		sys_ret(r, (unsigned long)(idx + 3));
		return;
	}

	const struct ramfs_file *file = ramfs_lookup(path);
	if (!file) {
		sys_ret(r, (unsigned long)-ENOENT);
		return;
	}
	int idx = process_fd_alloc();
	if (idx < 0) {
		sys_ret(r, (unsigned long)-ENOMEM); /* table full -- real errno would be EMFILE/ENFILE, close enough here */
		return;
	}
	struct fd_entry *fd = process_fd_get(idx);
	fd->data = file->data;
	fd->size = file->size;
	fd->pos = 0;
	fd->is_dir = 0;
	fd->dynfile = 0;
	sys_ret(r, (unsigned long)(idx + 3));
}

void sys_openat(struct regs *r) {
	/* arg0=dirfd, arg1=path, arg2=flags, arg3=mode (mode unused -- see
	 * this file's own header comment, permission bits are not
	 * modeled). */
	openat_core(r, (long)sys_arg(r, 0), (const char *)sys_arg(r, 1), sys_arg(r, 2));
}

/* i386 only (arch/i386/syscall.c's dispatch) -- real Linux i386 keeps
 * the legacy 3-argument SYS_open (path, flags, mode) alongside
 * SYS_openat, and musl's own src/fcntl/open.c prefers it whenever
 * SYS_open is defined for the target arch (confirmed in musl-i386's
 * own generated bits/syscall.h), which it is for i386 but isn't for
 * riscv64 (same "genuinely newer arch" reasoning as sys_unlink's own
 * comment). No dirfd argument at all here, not just an implied
 * AT_FDCWD -- openat_core() takes it as a parameter for exactly this
 * reason. */
void sys_open(struct regs *r) {
	openat_core(r, AT_FDCWD, (const char *)sys_arg(r, 0), sys_arg(r, 1));
}

void sys_close(struct regs *r) {
	unsigned long fd = sys_arg(r, 0);
	if (fd == 0 || fd == 1 || fd == 2) {
		/* checkpoint 12: if this fd is currently redirected (real
		 * shell I/O redirection -- process.h's own stdio_override
		 * comment), closing it reverts to the plain console; a no-op
		 * otherwise, same as before -- either way "honest enough,
		 * nothing real to release" (this kernel's fd 0/1/2 are always
		 * valid, there's no genuine closed state to model). */
		process_stdio_clear((int)fd);
		sys_ret(r, 0);
		return;
	}
	if (fd < 3 || !process_fd_get((int)fd - 3)) {
		sys_ret(r, (unsigned long)-EBADF);
		return;
	}
	process_fd_close((int)fd - 3);
	sys_ret(r, 0);
}

/* checkpoint 8: real blocking stdin read (fd 0) -- spin-yield until a
 * byte is available, same "cooperative block" idea as
 * SYS_sched_yield/SYS_wait4's own spin loops, then a single-byte read.
 * Returning fewer bytes than requested is POSIX-legal (read() has
 * never promised to fill the buffer); one raw byte at a time, no
 * echo/line-editing (no tty/line-discipline layer exists yet -- an
 * honest, documented gap, not a bug: whatever's on the other end of
 * the UART/serial console is responsible for its own local echo for
 * now). fd>=3 reads from mm/ramfs.h via this process's own fd table
 * instead. */
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

void sys_read(struct regs *r) {
	unsigned long fd = sys_arg(r, 0);
	unsigned char *buf = (unsigned char *)sys_arg(r, 1);
	unsigned long count = sys_arg(r, 2);

	if (fd == 0) {
		if (count == 0) {
			sys_ret(r, 0);
			return;
		}
		/* real shell input redirection (`command < file`) -- ash
		 * dup3()'d a real file onto fd 0 (see sys_dup3's own comment);
		 * read through it instead of ever touching the console. */
		struct fd_entry *ov = process_stdio_get(0);
		if (ov) {
			sys_ret(r, read_from_fd_entry(ov, buf, count));
			return;
		}
		while (!serial_rx_ready())
			process_schedule();
		paging_ensure_writable((unsigned long)buf, 1); /* see mm/paging_common.c's own comment -- real bug found here first */
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
		 * layer to put this in otherwise (this function's own header
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
		sys_ret(r, 1);
		return;
	}
	if (fd == 1 || fd == 2) {
		sys_ret(r, (unsigned long)-EBADF); /* stdout/stderr aren't readable */
		return;
	}

	struct fd_entry *entry = process_fd_get((int)fd - 3);
	if (!entry) {
		sys_ret(r, (unsigned long)-EBADF);
		return;
	}
	sys_ret(r, read_from_fd_entry(entry, buf, count));
}

/* checkpoint 12: real seeking -- needed for real file writes, not
 * just reads: compiler/tccelf.c's own ELF writer lays out sections at
 * their real file offsets via repeated lseek()+write() pairs, not
 * strictly-increasing sequential writes (confirmed by reading it, not
 * assumed -- see mm/ramfs.c's own ramfs_dynamic_write() comment on the
 * sparse-file "hole" this implies). Works the same for a read-only
 * fixed-table fd too (entry->dynfile is 0 there, entry->size is
 * already correct), no separate case needed. */
void sys_lseek(struct regs *r) {
	unsigned long fd = sys_arg(r, 0);
	long offset = (long)sys_arg(r, 1);
	unsigned long whence = sys_arg(r, 2);

	struct fd_entry *entry = fd >= 3 ? process_fd_get((int)fd - 3) : 0;
	if (!entry) {
		sys_ret(r, (unsigned long)-EBADF);
		return;
	}
	unsigned long size = entry->dynfile ? entry->dynfile->size : entry->size;
	long new_pos;
	switch (whence) {
		case SEEK_SET: new_pos = offset; break;
		case SEEK_CUR: new_pos = (long)entry->pos + offset; break;
		case SEEK_END: new_pos = (long)size + offset; break;
		default: sys_ret(r, (unsigned long)-EINVAL); return;
	}
	if (new_pos < 0) {
		sys_ret(r, (unsigned long)-EINVAL);
		return;
	}
	entry->pos = (unsigned long)new_pos;
	sys_ret(r, entry->pos);
}

/* checkpoint 12: real unlink -- neither arch's Linux ABI has a plain
 * SYS_unlink in this kernel's syscall set, only SYS_unlinkat (musl's
 * own src/unistd/unlink.c: unlink(path) is just
 * unlinkat(AT_FDCWD, path, 0) on an arch with no SYS_unlink defined,
 * confirmed by reading it). arg0=dirfd, arg1=path, arg2=flags --
 * dirfd/flags ignored, same "no real cwd" reasoning as sys_openat.
 * Best-effort by design (see mm/ramfs.h's own comment on
 * ramfs_dynamic_unlink()): a no-op, not an error, if no dynamic file
 * by that name exists -- exactly what compiler/tccelf.c's own
 * unlink()-before-create pattern needs (it never checks the return
 * value either way). */
static void unlink_user_path(struct regs *r, const char *user_path) {
	char path[PATH_MAX_LOCAL];
	resolve_user_path(path, user_path);
	ramfs_dynamic_unlink(path);
	sys_ret(r, 0);
}

void sys_unlinkat(struct regs *r) {
	unlink_user_path(r, (const char *)sys_arg(r, 1));
}

/* i386 only (arch/i386/syscall.c's dispatch) -- real Linux i386 keeps
 * the legacy single-argument SYS_unlink (10) alongside SYS_unlinkat
 * (301), and musl's own src/unistd/unlink.c prefers it whenever
 * SYS_unlink is defined for the target arch, which it is for i386
 * (confirmed in musl-i386's own generated bits/syscall.h) but isn't
 * for riscv64 (a genuinely newer arch, added to Linux long after
 * unlinkat() subsumed unlink() -- see sys_unlinkat's own comment).
 * Same underlying ramfs_dynamic_unlink() either way, just a different
 * argument index for the path (arg0, not unlinkat's arg1). */
void sys_unlink(struct regs *r) {
	unlink_user_path(r, (const char *)sys_arg(r, 0));
}

/* checkpoint 12: found running real `rm` (busybox coreutils, checking
 * a target actually exists before removing it) -- arg0=dirfd,
 * arg1=path, arg2=mode, arg3=flags, all ignored (same "no real
 * cwd/permissions model" reasoning as sys_openat/sys_newfstatat: this
 * ramfs doesn't model real permission bits, so the only thing
 * access() can honestly answer here is "does this path exist at
 * all", which is what every caller in this codebase actually needs it
 * for). Dynamic files take priority over the fixed table, same
 * reasoning as everywhere else. */
static void access_user_path(struct regs *r, const char *user_path) {
	char path[PATH_MAX_LOCAL];
	resolve_user_path(path, user_path);
	if (ramfs_is_dir(path)) {
		sys_ret(r, 0);
		return;
	}
	if (ramfs_dynamic_lookup(path) || ramfs_lookup(path)) {
		sys_ret(r, 0);
		return;
	}
	sys_ret(r, (unsigned long)-ENOENT);
}

void sys_faccessat(struct regs *r) {
	access_user_path(r, (const char *)sys_arg(r, 1));
}

/* i386 only (arch/i386/syscall.c's dispatch) -- real Linux i386 keeps
 * the legacy 2-argument SYS_access (33) alongside SYS_faccessat, and
 * musl's own src/unistd/access.c prefers it whenever SYS_access is
 * defined for the target arch (confirmed in musl-i386's own generated
 * bits/syscall.h), which it is for i386 but isn't for riscv64 -- same
 * "genuinely newer arch" reasoning as sys_fork's/sys_open's own
 * comments. */
void sys_access(struct regs *r) {
	access_user_path(r, (const char *)sys_arg(r, 0));
}

/* checkpoint 12: real shell I/O redirection (`echo x > file`) --
 * ash forks, opens the target file (a real fd >= 3), dup3()s it onto
 * fd 1, closes the original, *then* runs the actual command, which
 * just writes to fd 1 as always. See process.h's own stdio_override
 * comment for the real simplification this makes (copies fd state
 * into the target slot rather than making the two fds genuinely
 * share one open-file position -- correct for this dominant
 * open-dup-close pattern). */
static void dup_into(struct regs *r, unsigned long oldfd, unsigned long newfd) {
	struct fd_entry *src = oldfd <= 2 ? process_stdio_get((int)oldfd) : process_fd_get((int)oldfd - 3);
	if (!src) {
		sys_ret(r, (unsigned long)-EBADF); /* oldfd is the plain (non-redirected) console, or genuinely invalid -- nothing to duplicate */
		return;
	}

	if (newfd <= 2) {
		process_stdio_set((int)newfd, src);
	} else {
		int idx = (int)newfd - 3;
		if (idx >= MAX_FDS) {
			sys_ret(r, (unsigned long)-EBADF);
			return;
		}
		process_fd_set(idx, src);
	}
	sys_ret(r, newfd);
}

void sys_dup3(struct regs *r) {
	unsigned long oldfd = sys_arg(r, 0);
	unsigned long newfd = sys_arg(r, 1);
	/* arg 2 (flags, O_CLOEXEC) ignored -- no real fd-flags model */
	if (oldfd == newfd) {
		sys_ret(r, (unsigned long)-EINVAL); /* real dup3()'s own rule */
		return;
	}
	dup_into(r, oldfd, newfd);
}

/* i386 only (arch/i386/syscall.c's dispatch) -- real Linux i386 keeps
 * the legacy 2-argument SYS_dup2 (63) alongside SYS_dup3, and musl's
 * own src/unistd/dup2.c prefers it whenever SYS_dup2 is defined for
 * the target arch (confirmed in musl-i386's own generated
 * bits/syscall.h), which it is for i386 but isn't for riscv64 -- same
 * "genuinely newer arch" reasoning as sys_fork's own comment. Unlike
 * dup3(), dup2(old,old) is a defined, successful no-op (real Linux:
 * returns `old` unchanged, doesn't even require it be a valid fd on
 * every kernel version, but requiring it here -- via still routing
 * through dup_into() -- is the honest answer for a fd this kernel
 * doesn't recognize at all). */
void sys_dup2(struct regs *r) {
	unsigned long oldfd = sys_arg(r, 0);
	unsigned long newfd = sys_arg(r, 1);
	if (oldfd == newfd) {
		dup_into(r, oldfd, oldfd); /* validates oldfd, then "copies" it onto itself -- a correct no-op */
		return;
	}
	dup_into(r, oldfd, newfd);
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
void sys_fcntl(struct regs *r) {
	unsigned long fd = sys_arg(r, 0);
	unsigned long cmd = sys_arg(r, 1);

	if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
		if (fd < 3) {
			/* dup'ing the console: no real second console fd to hand
			 * back (fd 0/1/2 aren't reference-counted objects here at
			 * all) -- reporting success as the same fd is honest
			 * enough for what this checkpoint exercises. */
			sys_ret(r, fd);
			return;
		}
		struct fd_entry *old = process_fd_get((int)fd - 3);
		if (!old) {
			sys_ret(r, (unsigned long)-EBADF);
			return;
		}
		int idx = process_fd_alloc();
		if (idx < 0) {
			sys_ret(r, (unsigned long)-ENOMEM);
			return;
		}
		struct fd_entry *nf = process_fd_get(idx);
		nf->data = old->data;
		nf->size = old->size;
		nf->pos = old->pos;
		nf->is_dir = old->is_dir;
		nf->dynfile = old->dynfile;
		sys_ret(r, (unsigned long)(idx + 3));
		return;
	}
	if (cmd == F_GETFD || cmd == F_SETFD || cmd == F_SETFL) {
		sys_ret(r, 0);
		return;
	}
	if (cmd == F_GETFL) {
		sys_ret(r, 0); /* O_RDONLY -- every fd this kernel hands out is read-only (mm/ramfs.h) */
		return;
	}
	sys_ret(r, (unsigned long)-ENOSYS);
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
 * calls those, so it's left 0 rather than computed. Same layout on
 * both arches -- this is the Linux kernel ABI struct, not something
 * either arch's own struct dirent lays out differently. */
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

void sys_getdents64(struct regs *r) {
	unsigned long fd_num = sys_arg(r, 0);
	unsigned char *buf = (unsigned char *)sys_arg(r, 1);
	unsigned long count = sys_arg(r, 2);

	struct fd_entry *fd = fd_num >= 3 ? process_fd_get((int)fd_num - 3) : 0;
	if (!fd) {
		sys_ret(r, (unsigned long)-EBADF);
		return;
	}
	if (!fd->is_dir) {
		sys_ret(r, (unsigned long)-ENOTDIR);
		return;
	}

	/* fd->pos indexes ".", "..", then the directory's unique immediate
	 * children synthesized from ramfs path prefixes. */
	unsigned long written = 0;
	while (1) {
		char name[PATH_MAX_LOCAL];
		unsigned char d_type;
		if (fd->pos == 0) { name[0] = '.'; name[1] = 0; d_type = DT_DIR; }
		else if (fd->pos == 1) { name[0] = '.'; name[1] = '.'; name[2] = 0; d_type = DT_DIR; }
		else {
			int is_dir, is_symlink;
			if (!ramfs_dir_entry(fd->path, (unsigned int)fd->pos - 2, name,
			    sizeof(name), &is_dir, &is_symlink)) break;
			d_type = is_dir ? DT_DIR : is_symlink ? DT_LNK : DT_REG;
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
	sys_ret(r, written); /* 0 once fd->pos reaches total -- musl's readdir() treats that as EOF */
}

/* checkpoint 8: real execve() -- sched/process.h's process_execve()
 * does the actual work (new address space, new stack, rewrites the
 * live trapframe in place); this just copies argv/envp's user-space
 * pointer arrays into kernel memory first (each element is itself a
 * user pointer, walked here rather than inside process_execve() so
 * that function's own signature can just be "two NUL-terminated
 * char* arrays", arch-neutral in spirit even though nothing else
 * uses it yet). */
/* checkpoint 14: bumped from 8 -- must match sched/process.c's own
 * copy (duplicated rather than shared via a header, same convention
 * as that file's own FORK_MMAP_HI comment). A real self-hosted
 * `tcc -B... -I... -I... -I... -I... -nostdinc -c -o out.o in.c`
 * invocation (see compiler/Makefile's own stage1 recipe, the proven
 * command line this mirrors) needs 13 argv entries; 20 is headroom
 * above that measured count. */
#define EXECVE_MAX_ARGV 20

void sys_execve(struct regs *r) {
	char path[PATH_MAX_LOCAL];
	resolve_user_path(path, (const char *)sys_arg(r, 0));

	char *argv[EXECVE_MAX_ARGV + 1];
	unsigned long *user_argv = (unsigned long *)sys_arg(r, 1);
	int argc = 0;
	if (user_argv)
		while (argc < EXECVE_MAX_ARGV && user_argv[argc]) {
			argv[argc] = (char *)user_argv[argc];
			argc++;
		}
	argv[argc] = 0;

	int ret = process_execve(r, path, argv, 0);
	if (ret < 0)
		sys_ret(r, (unsigned long)-ENOENT);
	/* on success, process_execve() already rewrote every register
	 * that matters -- the return value here is meaningless either way
	 * (execve() doesn't "return" 0 on success, it just doesn't return
	 * to this call site at all) */
}

/* checkpoint 9: busybox ash's own startup needs all of these -- see
 * arch/risc/riscv64_syscall.c's own git history for the real strace
 * this was derived from (a real ash -c/script run under
 * qemu-riscv64-static, same methodology as everything else in this
 * file).
 *
 * struct stat layout confirmed by compiling a small offsetof() probe
 * with the riscv64 toolchain and running it under qemu-riscv64-static,
 * rather than hand-deriving field offsets from musl's typedefs
 * (nlink_t/blksize_t/etc.'s actual sizes depend on ifdef branches easy
 * to misread): dev_t/ino_t/rdev at byte offsets 0/8/32 (8 bytes
 * each), mode_t/nlink_t/uid_t/gid_t at 16/20/24/28 (4 bytes each), an
 * 8-byte pad, then off_t/blksize_t/blkcnt_t at 48/56/64 (8 bytes
 * each), three 16-byte timespecs from byte 72 -- 128 bytes total.
 * i386's own struct stat (musl's arch/i386/bits/stat.h) is a
 * *different* real layout, not yet confirmed the same rigorous way --
 * sys_newfstatat is deliberately not shared for that reason, see its
 * own comment right below. */
#define S_IFDIR 0040000
#define S_IFREG 0100000

void sys_getcwd(struct regs *r) {
	char *buf = (char *)sys_arg(r, 0);
	unsigned long size = sys_arg(r, 1);
	const char *cwd = process_current_cwd();
	unsigned long len = 0;
	while (cwd[len]) len++;
	if (size <= len) {
		sys_ret(r, (unsigned long)-EINVAL); /* ERANGE would be more precise; not worth a new errno for this */
		return;
	}
	paging_ensure_writable((unsigned long)buf, len + 1);
	for (unsigned long i = 0; i <= len; i++) buf[i] = cwd[i];
	sys_ret(r, len + 1);
}

void sys_chdir(struct regs *r) {
	char path[PATH_MAX_LOCAL], absolute[PATH_MAX_LOCAL];
	resolve_user_path(path, (const char *)sys_arg(r, 0));
	if (!ramfs_is_dir(path)) { sys_ret(r, (unsigned long)-ENOENT); return; }
	unsigned int n = 0;
	absolute[n++] = '/';
	for (unsigned int i = 0; path[i] && n < sizeof(absolute) - 1; i++) absolute[n++] = path[i];
	absolute[n] = 0;
	process_set_current_cwd(absolute);
	sys_ret(r, 0);
}

/* Honest stub, same spirit as sys_rt_sigprocmask -- no real signal
 * delivery exists yet, so there's nothing to actually install; ash's
 * own startup installs handlers for SIGINT/SIGQUIT/SIGTERM/SIGCHLD
 * unconditionally and needs this to merely succeed, not actually
 * work (real signal delivery -- e.g. Ctrl-C interrupting a running
 * command -- is future scope). */
void sys_rt_sigaction(struct regs *r) {
	sys_ret(r, 0);
}

/* arg0=dirfd, arg1=path, arg2=statbuf, arg3=flags. Kept out of
 * syscall_posix.c's shared set (unlike every other handler here) --
 * see this file's own struct-stat comment just above sys_getcwd():
 * i386's struct stat layout hasn't been independently confirmed the
 * same rigorous way riscv64's was, and getting it wrong silently
 * (rather than failing to link) is a worse failure mode than a few
 * duplicated lines. Each arch's own syscall.c still implements this
 * one itself. */
