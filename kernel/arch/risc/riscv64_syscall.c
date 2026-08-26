/* Syscall dispatch, Linux riscv64 ABI: a7=number, a0-a5=args 1-6,
 * return value in a0. Same "derived from a real strace, not guessed"
 * methodology as arch/i386/syscall.c -- these are exactly the syscalls
 * `qemu-riscv64-static -strace` showed our own musl-linked riscv64
 * hello binary (from the compiler-port work) actually calling on its
 * way to main() and back: set_tid_address, brk, mmap, munmap, ioctl,
 * writev, exit_group. See docs/riscv-port-findings.md. sched_yield
 * (checkpoint 6), clone/wait4/rt_sigprocmask (checkpoint 7) were added
 * the same way, later -- each syscall's own comment (now in
 * syscall_posix.c, where the actual handler bodies live) says which
 * real binary and real strace it came from.
 *
 * Notably shorter than i386's number list: riscv64 needs no
 * set_thread_area (TLS is just the `tp` register, no syscall
 * involved) and, being a genuinely newer Linux port, never had most
 * of i386's legacy syscalls (SYS_open/SYS_fork/SYS_dup2/SYS_access/
 * SYS_stat/SYS_getuid32/...) to begin with -- only ever the *at()-
 * suffixed/64-bit-safe modern forms, one number per operation instead
 * of i386's two. See arch/i386/syscall.c's own header comment for the
 * full accounting of why its list is longer despite sharing almost
 * all the underlying *logic* (syscall_posix.c) with this file.
 *
 * checkpoint 18 (docs/kernel-arch-split-plan.md, "genericize rather
 * than write afresh"): this file used to be the full ~1100-line
 * implementation of every handler below -- once arch/i386/syscall.c
 * grew a real process table to actually run them against (checkpoint
 * 17, arch/i386/process.c) and needed almost the same set, comparing
 * the two showed the handler bodies never actually depended on *how*
 * an argument got read out of `r`, only on being able to read it at
 * all. Everything that turned out to be genuinely identical moved to
 * syscall_posix.c; this file now owns exactly the syscall numbers
 * below, the sys_arg()/sys_ret() accessors syscall_posix.c calls
 * through, and sys_newfstatat (riscv64's own struct stat layout is a
 * different real layout from i386's -- see arch/i386/syscall.c's own
 * comment on why that one specific handler stays per-arch). */
#include "kernel.h"
#include "riscv64_trap.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/ramfs.h"
#include "sched/process.h"
#include "syscall_common.h"

#define SYS_ioctl             29
#define SYS_sched_yield      124
#define SYS_write              64
#define SYS_writev              66
#define SYS_exit                  93
#define SYS_exit_group              94
#define SYS_set_tid_address           96
#define SYS_brk                         214
#define SYS_mremap                        216
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

#define ENOSYS  38
#define ENOENT   2

/* --- register accessors syscall_posix.c calls through --- */
unsigned long sys_arg(struct regs *r, int n) {
	switch (n) {
	case 0: return r->a0;
	case 1: return r->a1;
	case 2: return r->a2;
	case 3: return r->a3;
	case 4: return r->a4;
	case 5: return r->a5;
	}
	return 0;
}

void sys_ret(struct regs *r, unsigned long val) {
	r->a0 = val;
}

#define PATH_MAX_LOCAL 128

/* checkpoint 9: busybox ash's own startup needs this -- see this
 * file's own git history for the real strace this was derived from (a
 * real ash -c/script run under qemu-riscv64-static, same methodology
 * as everything else in this file).
 *
 * struct stat layout confirmed by compiling a small offsetof() probe
 * with the riscv64 toolchain and running it under qemu-riscv64-static,
 * rather than hand-deriving field offsets from musl's typedefs
 * (nlink_t/blksize_t/etc.'s actual sizes depend on ifdef branches easy
 * to misread): dev_t/ino_t/rdev at byte offsets 0/8/32 (8 bytes
 * each), mode_t/nlink_t/uid_t/gid_t at 16/20/24/28 (4 bytes each), an
 * 8-byte pad, then off_t/blksize_t/blkcnt_t at 48/56/64 (8 bytes
 * each), three 16-byte timespecs from byte 72 -- 128 bytes total.
 * Only st_mode and st_size are ever read by anything this kernel runs
 * (mm/ramfs.h's ash-facing PATH search checks S_ISREG(st_mode) and
 * nothing else -- confirmed in ash.c's own source, not guessed), so
 * everything else here is just zeroed rather than computed to match.
 * i386's own struct stat/kstat layout is a genuinely different real
 * layout, not shared -- see arch/i386/syscall.c's own comment on its
 * sys_newfstatat. */
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

static int copy_path_from_user(char *dst, const char *user_src) {
	int i = 0;
	while (user_src[i] && i < PATH_MAX_LOCAL - 1) {
		dst[i] = user_src[i];
		i++;
	}
	dst[i] = 0;
	return i;
}

/* Canonicalize absolute or cwd-relative paths into the ramfs key form
 * -- same logic as syscall_posix.c's own (private, static) version,
 * duplicated rather than shared for the same reason arch/i386/syscall.c's
 * own sys_newfstatat duplicates it: this is the one place in this
 * file that needs it. */
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

static void sys_newfstatat(struct regs *r) {
	/* a0=dirfd, a1=path, a2=statbuf, a3=flags. */
	char path[PATH_MAX_LOCAL];
	resolve_user_path(path, (const char *)sys_arg(r, 1));
	unsigned char *sb = (unsigned char *)sys_arg(r, 2);
	paging_ensure_writable((unsigned long)sb, 128); /* sizeof(struct stat) -- see fill_stat()'s own comment for the layout */

	if (ramfs_is_dir(path)) {
		fill_stat(sb, S_IFDIR | 0755, 0);
		sys_ret(r, 0);
		return;
	}

	/* checkpoint 12: dynamic files take priority over the fixed table,
	 * same reasoning as sys_openat's own lookup order -- a freshly
	 * written file should stat() as itself, not (if it happens to
	 * share a name) whatever fixed entry existed first. */
	struct ramfs_dynamic_file *dyn = ramfs_dynamic_lookup(path);
	if (dyn) {
		fill_stat(sb, S_IFREG | 0755, dyn->size);
		sys_ret(r, 0);
		return;
	}

	const struct ramfs_file *file = ramfs_lookup(path);
	if (!file) {
		sys_ret(r, (unsigned long)-ENOENT);
		return;
	}
	fill_stat(sb, S_IFREG | 0755, file->size);
	sys_ret(r, 0);
}

static void syscall_dispatch(struct regs *r) {
	switch (r->a7) {
	case SYS_write:            sys_write(r); return;
	case SYS_writev:           sys_writev(r); return;
	case SYS_exit:              sys_exit(r); return;
	case SYS_exit_group:        sys_exit_group(r); return;
	case SYS_brk:                sys_brk(r); return;
	case SYS_mremap:                sys_mremap(r); return;
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
		"brk, mmap, mremap, munmap, ioctl, sched_yield, set_tid_address, "
		"clone, wait4, rt_sigprocmask, gettid, openat, close, read, execve, "
		"getcwd, chdir, newfstatat, rt_sigaction, getppid, geteuid, "
		"getpid, fcntl, getdents64, lseek, unlinkat, dup3, faccessat)\n");
}
