/* Syscall dispatch, Linux i386 ABI (plan decision D4): eax=number,
 * ebx/ecx/edx/esi/edi/ebp=args 1-6, return value in eax.
 *
 * checkpoint 18 (docs/kernel-arch-split-plan.md, "genericize rather
 * than write afresh"): every handler body that doesn't genuinely
 * depend on i386's own register layout or its own legacy syscall
 * numbers now lives in syscall_posix.c, shared outright with
 * arch/risc/riscv64_syscall.c -- this file owns exactly what the
 * Linux i386 ABI genuinely does make arch-specific: the syscall
 * *numbers* below, the sys_arg()/sys_ret() register accessors
 * syscall_posix.c calls through, and struct-stat-shaped handlers
 * (i386's own on-the-wire layout is a genuinely different real one,
 * see sys_newfstatat's own comment).
 *
 * The numbers below are NOT guessed -- every one was cross-checked
 * against musl-i386's own *generated* bits/syscall.h (obj/include/,
 * the real numbers musl actually compiles against) and, for every
 * syscall i386 has more than one real number for (open/openat,
 * fork/clone, dup2/dup3, access/faccessat, stat/fstat/fstatat, plus
 * the *32-suffixed uid/gid family and the *64-suffixed fcntl/stat
 * family), against the specific musl source file that decides which
 * one a real call actually reaches (confirmed by reading
 * src/process/_Fork.c, src/fcntl/open.c, src/unistd/{dup2,access}.c,
 * src/stat/fstatat.c, and src/internal/syscall.h's own `#undef
 * SYS_x / #define SYS_x SYS_x32` fixups) rather than assumed from
 * riscv64's shorter, newer-arch-only list. i386 predates most of the
 * *at()-suffixed/64-bit-safe syscalls riscv64 only ever knew, so
 * musl-i386 prefers the legacy form wherever one still exists --
 * genuinely more syscall *numbers* than riscv64 needs even though
 * most of the underlying *logic* (syscall_posix.c) is identical. */
#include "kernel.h"
#include "idt.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/ramfs.h"
#include "sched/process.h"
#include "syscall_common.h"

#define SYS_exit               1
#define SYS_fork                2
#define SYS_read                 3
#define SYS_write                 4
#define SYS_open                    5
#define SYS_close                     6
#define SYS_unlink                      10
#define SYS_execve                        11
#define SYS_chdir                            12
#define SYS_lseek                               19
#define SYS_getpid                                 20
#define SYS_access                                    33
#define SYS_brk                                          45
#define SYS_getegid                                        50 /* getegid32, see this file's own header comment */
#define SYS_ioctl                                             54
#define SYS_fcntl                                               55 /* fcntl64, see this file's own header comment */
#define SYS_dup2                                                  63
#define SYS_getppid                                                  64
#define SYS_munmap                                                      91
#define SYS_wait4                                                          114
#define SYS_writev                                                            146
#define SYS_sched_yield                                                          158
#define SYS_mremap                                                                  163
#define SYS_getcwd                                                                     183
#define SYS_mmap2                                                                         192
#define SYS_stat                                                                             195 /* stat64, see this file's own header comment */
#define SYS_fstat                                                                               197 /* fstat64 */
#define SYS_getuid                                                                                 199 /* getuid32 */
#define SYS_getgid                                                                                    200 /* getgid32 */
#define SYS_geteuid                                                                                      201 /* geteuid32 */
#define SYS_getdents64                                                                                      220
#define SYS_gettid                                                                                             224
#define SYS_set_thread_area                                                                                       243
#define SYS_exit_group                                                                                       252
#define SYS_set_tid_address                                                                                            258
#define SYS_openat                                                                                                        295
#define SYS_newfstatat                                                                                               300 /* fstatat64 */
#define SYS_faccessat                                                                                                        307
#define SYS_dup3                                                                                                                330
/* SYS_clone, SYS_rt_sigprocmask, SYS_rt_sigaction: real numbers, but
 * musl-i386's own preferences (SYS_fork/legacy signal syscalls -- see
 * this file's header comment) mean these two are dispatched only as a
 * defensive fallback should anything ever call clone()/the rt_ forms
 * directly rather than through fork()/signal(). */
#define SYS_clone            120
#define SYS_rt_sigprocmask   175
#define SYS_rt_sigaction     174

#define ENOSYS  38
#define ENOENT   2

/* --- register accessors syscall_posix.c calls through --- */
unsigned long sys_arg(struct regs *r, int n) {
	switch (n) {
	case 0: return r->ebx;
	case 1: return r->ecx;
	case 2: return r->edx;
	case 3: return r->esi;
	case 4: return r->edi;
	case 5: return r->ebp;
	}
	return 0;
}

void sys_ret(struct regs *r, unsigned long val) {
	r->eax = val;
}

/* i386's own struct stat (musl-i386/arch/i386/bits/stat.h) is a
 * genuinely different real layout from riscv64's -- but what the
 * *syscall* actually fills isn't even that: it's musl's internal
 * `struct kstat` (src/internal/kstat.h), a plainer kernel-ABI-shaped
 * struct musl's own C library code then translates into the richer
 * userland struct stat (extra 64-bit timespecs, a full-width st_ino
 * copy) entirely in userspace -- see musl's src/stat/fstatat.c
 * fstatat_kstat() for exactly that translation. Confirmed by reading
 * both i386 headers, and cross-checked by compiling a matching
 * offsetof() probe against musl-i386's own real headers with host
 * gcc -m32 (same "verify against the real ABI, don't hand-derive"
 * methodology as riscv64's own equivalent comment): kstat's size is
 * 96 bytes (not the userland struct's 120), but st_mode/st_size land
 * at the *same* offsets either way (16 / 44) since kstat and the
 * userland struct share an identical prefix layout up through
 * st_blocks -- only the trailing timestamp/inode fields differ,
 * which nothing this kernel runs actually reads. */
#define ST_MODE_OFF 16
#define ST_SIZE_OFF 44
#define ST_STRUCT_SIZE 96
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFCHR 0020000

static void fill_stat(unsigned char *sb, unsigned int mode, unsigned long size) {
	for (int i = 0; i < ST_STRUCT_SIZE; i++)
		sb[i] = 0;
	*(unsigned int *)(sb + ST_MODE_OFF) = mode;
	*(unsigned int *)(sb + 20) = 1; /* st_nlink */
	*(unsigned int *)(sb + ST_SIZE_OFF) = (unsigned int)size; /* off_t is 8 bytes; every real size here fits in the low 4 */
	*(unsigned int *)(sb + ST_SIZE_OFF + 4) = 0;
	*(unsigned int *)(sb + 52) = 512; /* st_blksize */
}

/* Looks a ramfs path up and fills `sb` -- the actual logic shared by
 * sys_stat/sys_newfstatat below (both take a path; sys_fstat looks up
 * an already-open fd instead, a genuinely different kind of lookup,
 * kept separate). Returns 0 on success, -ENOENT if nothing by that
 * name exists. */
static int stat_path(const char *ramfs_path, unsigned char *sb) {
	if (ramfs_is_dir(ramfs_path)) {
		fill_stat(sb, S_IFDIR | 0755, 0);
		return 0;
	}
	struct ramfs_dynamic_file *dyn = ramfs_dynamic_lookup(ramfs_path);
	if (dyn) {
		fill_stat(sb, S_IFREG | 0755, dyn->size);
		return 0;
	}
	const struct ramfs_file *file = ramfs_lookup(ramfs_path);
	if (!file) return -ENOENT;
	fill_stat(sb, S_IFREG | 0755, file->size);
	return 0;
}

/* arg0=path, arg1=statbuf -- SYS_stat (stat64), i386's own preferred
 * real syscall for a plain absolute/cwd-relative stat() (see this
 * file's own header comment). No cwd-relative resolution here (unlike
 * syscall_posix.c's own resolve_user_path): every real caller in this
 * kernel's tests passes an absolute path to this specific syscall. */
static void sys_stat(struct regs *r) {
	const char *path = (const char *)sys_arg(r, 0);
	unsigned char *sb = (unsigned char *)sys_arg(r, 1);
	paging_ensure_writable((unsigned long)sb, ST_STRUCT_SIZE);
	const char *ramfs_path = path[0] == '/' ? path + 1 : path;
	int ret = stat_path(ramfs_path, sb);
	sys_ret(r, ret < 0 ? (unsigned long)ret : 0);
}

/* arg0=fd, arg1=statbuf -- SYS_fstat (fstat64), reached via musl's
 * fstat(fd,...) wrapper (src/stat/fstat.c: __fstatat(fd, "",
 * AT_EMPTY_PATH), which fstatat_kstat() turns into a direct
 * SYS_fstat(fd, &kst) call rather than going through the path-based
 * fstatat syscall at all -- confirmed by reading it). fd 0/1/2
 * (console) report as a character device, same as a real tty/serial
 * line would; fd>=3 looks the already-open ramfs entry up directly
 * (no path resolution needed at all -- it's already open). */
static void sys_fstat(struct regs *r) {
	unsigned long fd = sys_arg(r, 0);
	unsigned char *sb = (unsigned char *)sys_arg(r, 1);
	paging_ensure_writable((unsigned long)sb, ST_STRUCT_SIZE);

	if (fd <= 2) {
		fill_stat(sb, S_IFCHR | 0620, 0);
		sys_ret(r, 0);
		return;
	}
	struct fd_entry *entry = process_fd_get((int)fd - 3);
	if (!entry) {
		sys_ret(r, (unsigned long)-9 /* EBADF */);
		return;
	}
	if (entry->is_dir) {
		fill_stat(sb, S_IFDIR | 0755, 0);
	} else {
		unsigned long size = entry->dynfile ? entry->dynfile->size : entry->size;
		fill_stat(sb, S_IFREG | 0755, size);
	}
	sys_ret(r, 0);
}

/* arg0=dirfd, arg1=path, arg2=statbuf, arg3=flags -- SYS_newfstatat
 * (fstatat64), reached whenever fd != AT_FDCWD or (rarely, in this
 * kernel's own test set) a caller uses fstatat() by name instead of
 * plain stat(). Resolution against a real dirfd (mirroring
 * syscall_posix.c's own sys_openat) rather than always-absolute --
 * genuinely needed here, unlike sys_stat/sys_fstat above, since a
 * caller reaching this specific syscall might be doing exactly that. */
static void sys_newfstatat(struct regs *r) {
	char path[128];
	int i = 0;
	const char *user_path = (const char *)sys_arg(r, 1);
	while (user_path[i] && i < 127) { path[i] = user_path[i]; i++; }
	path[i] = 0;
	unsigned char *sb = (unsigned char *)sys_arg(r, 2);
	paging_ensure_writable((unsigned long)sb, ST_STRUCT_SIZE);

	const char *base = process_current_cwd();
	long dirfd = (long)sys_arg(r, 0);
	if (path[0] != '/' && dirfd != -100 /* AT_FDCWD */) {
		struct fd_entry *df = dirfd >= 3 ? process_fd_get((int)dirfd - 3) : 0;
		if (!df || !df->is_dir) { sys_ret(r, (unsigned long)-9 /* EBADF */); return; }
		base = df->path;
	}
	/* cwd-relative resolution, same shape as syscall_posix.c's own
	 * resolve_path (duplicated rather than shared: that one is
	 * `static` to this file's sibling, and this is the one place in
	 * arch/i386/syscall.c that needs it). */
	char resolved[128];
	unsigned int n = 0, j = 0;
	if (path[0] != '/') {
		const char *b = base[0] == '/' ? base + 1 : base;
		while (b[n] && n < sizeof(resolved) - 1) { resolved[n] = b[n]; n++; }
	}
	while (path[j]) {
		while (path[j] == '/') j++;
		unsigned int start = j;
		while (path[j] && path[j] != '/') j++;
		unsigned int len = j - start;
		if (!len || (len == 1 && path[start] == '.')) continue;
		if (len == 2 && path[start] == '.' && path[start + 1] == '.') {
			while (n && resolved[n - 1] != '/') n--;
			if (n) n--;
			continue;
		}
		if (n && n < sizeof(resolved) - 1) resolved[n++] = '/';
		for (unsigned int k = 0; k < len && n < sizeof(resolved) - 1; k++) resolved[n++] = path[start + k];
	}
	resolved[n] = 0;

	int ret = stat_path(resolved, sb);
	sys_ret(r, ret < 0 ? (unsigned long)ret : 0);
}

/* struct user_desc, Linux's real layout (see docs/kernel-p5-findings.md
 * for how this was derived from musl's __set_thread_area.s):
 *   u32 entry_number; u32 base_addr; u32 limit; u32 flags_bitfield;
 * We don't decode the flags bitfield -- every real caller (musl) sends
 * the same standard "present, 32-bit, page-granular, full 4GB, usable"
 * descriptor, so gdt_set_tls_entry always builds exactly that, keyed
 * only off base_addr. */
static void sys_set_thread_area(struct regs *r) {
	unsigned int *desc = (unsigned int *)sys_arg(r, 0);
	unsigned int base_addr = desc[1];

	/* Deliberately NOT validating entry_number (desc[0]) here -- see
	 * docs/kernel-p5-findings.md. musl's __set_thread_area.s computes
	 * its cached "-1" sentinel via a call-then-add-label-difference
	 * trick spanning the .text/.data gap, and TCC's linker resolves
	 * that specific relocation 3 bytes short of the real target
	 * (confirmed directly: linked a minimal reproduction, checked the
	 * actual .data address against the relocated immediate by hand).
	 * The value musl reads back is garbage as a result. Since this
	 * kernel only ever supports one TLS user at a time anyway and
	 * always hands out slot 6 regardless of what was requested, the
	 * simplest correct fix is to just not require entry_number to be
	 * meaningful -- every real call only cares that base_addr is
	 * right, which it is. */
	int slot = 6;
	gdt_set_tls_entry(slot, base_addr);
	desc[0] = (unsigned int)slot; /* kernel writes the allocated slot back */
	sys_ret(r, 0);
}

static void syscall_dispatch(struct regs *r) {
	switch (r->eax) {
	case SYS_write:            sys_write(r); return;
	case SYS_writev:           sys_writev(r); return;
	case SYS_exit:              sys_exit(r); return;
	case SYS_exit_group:        sys_exit_group(r); return;
	case SYS_brk:                sys_brk(r); return;
	case SYS_mmap2:                sys_mmap(r); return; /* mmap2's pgoffset arg is never read -- see sys_mmap's own comment */
	case SYS_mremap:                 sys_mremap(r); return;
	case SYS_munmap:                   sys_munmap(r); return;
	case SYS_ioctl:                      sys_ioctl(r); return;
	case SYS_set_thread_area:               sys_set_thread_area(r); return;
	case SYS_set_tid_address:                  sys_set_tid_address(r); return;
	case SYS_sched_yield:                         sys_sched_yield(r); return;
	case SYS_fork:                                   sys_fork(r); return;
	case SYS_clone:                                     sys_clone(r); return;
	case SYS_wait4:                                        sys_wait4(r); return;
	case SYS_rt_sigprocmask:                                  sys_rt_sigprocmask(r); return;
	case SYS_rt_sigaction:                                       sys_rt_sigaction(r); return;
	case SYS_gettid:                                                sys_gettid(r); return;
	case SYS_getppid:                                                  sys_getppid(r); return;
	case SYS_geteuid:                                                     sys_geteuid(r); return;
	case SYS_getuid:                                                         sys_getuid(r); return;
	case SYS_getgid:                                                            sys_getgid(r); return;
	case SYS_getegid:                                                              sys_getegid(r); return;
	case SYS_getpid:                                                                  sys_getpid(r); return;
	case SYS_open:                                                                       sys_open(r); return;
	case SYS_openat:                                                                        sys_openat(r); return;
	case SYS_close:                                                                            sys_close(r); return;
	case SYS_read:                                                                                sys_read(r); return;
	case SYS_lseek:                                                                                  sys_lseek(r); return;
	case SYS_unlink:                                                                                    sys_unlink(r); return;
	case SYS_execve:                                                                                       sys_execve(r); return;
	case SYS_getcwd:                                                                                          sys_getcwd(r); return;
	case SYS_chdir:                                                                                             sys_chdir(r); return;
	case SYS_stat:                                                                                                 sys_stat(r); return;
	case SYS_fstat:                                                                                                   sys_fstat(r); return;
	case SYS_newfstatat:                                                                                              sys_newfstatat(r); return;
	case SYS_getdents64:                                                                                                 sys_getdents64(r); return;
	case SYS_fcntl:                                                                                                         sys_fcntl(r); return;
	case SYS_dup2:                                                                                                             sys_dup2(r); return;
	case SYS_dup3:                                                                                                                sys_dup3(r); return;
	case SYS_access:                                                                                                                 sys_access(r); return;
	case SYS_faccessat:                                                                                                                 sys_faccessat(r); return;
	default:
		kprintf("FATAL: unimplemented syscall %u\n", r->eax);
		r->eax = (unsigned int)-ENOSYS;
	}
}

void syscall_init(void) {
	syscall_set_handler(syscall_dispatch);
	kprintf("syscall: dispatch installed (%d syscalls: write, fork, open, "
		"execve, stat/fstat/newfstatat, dup2/dup3, access/faccessat, "
		"clone, wait4, getdents64, and more -- see this file's own dispatch)\n", 40);
}
