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
#define SYS_ioctl                                             54
#define SYS_dup2                                                  63
#define SYS_getppid                                                  64
#define SYS_munmap                                                      91
#define SYS_wait4                                                          114
#define SYS__llseek                                                           140 /* real bug, found running real self-hosted TCC for the first time -- see sys_llseek's own comment: musl-i386's own lseek() always uses this instead of plain SYS_lseek above */
#define SYS_writev                                                              146
#define SYS_sched_yield                                                          158
#define SYS_nanosleep                                                             162
#define SYS_mremap                                                                  163
#define SYS_getcwd                                                                     183
#define SYS_mmap2                                                                         192
#define SYS_stat                                                                             195 /* stat64, see this file's own header comment */
#define SYS_lstat                                                                             196 /* lstat64 */
#define SYS_fstat                                                                               197 /* fstat64 */
#define SYS_getuid                                                                                 199 /* getuid32 */
#define SYS_getgid                                                                                    200 /* getgid32 */
#define SYS_geteuid                                                                                      201 /* geteuid32 */
#define SYS_getegid                                                                                        202 /* getegid32 -- real bug, found running real busybox ash for the first time: was mistakenly 50 (a stray leftover), a real command's very first PATH lookup hit ash's own getegid() call and got a loud "unimplemented syscall" for a syscall that was actually dispatched, just to the wrong number */
#define SYS_getdents64                                                                                      220
#define SYS_fcntl                                                                                              221 /* fcntl64 -- real bug, found running real busybox ash's own `ls`: was mistakenly the legacy single-number 55 (never actually reached, same mistake as SYS_getegid above), so real fcntl()s (F_DUPFD_CLOEXEC on ash's own script fd, same as riscv64's own checkpoint 9) hit the loud default case instead */
#define SYS_gettid                                                                                             224
#define SYS_set_thread_area                                                                                       243
#define SYS_exit_group                                                                                       252
#define SYS_set_tid_address                                                                                            258
#define SYS_openat                                                                                                        295
#define SYS_newfstatat                                                                                               300 /* fstatat64 */
#define SYS_faccessat                                                                                                        307
#define SYS_dup3                                                                                                                330
#define SYS_pipe2                                                                                                               331
#define SYS_renameat2                                                                                                           353
/* Not dispatched to a handler at all -- see the default case's own
 * comment on why this one specific unimplemented number is silenced
 * rather than a real gap. */
#define SYS_statx                                                                                                                   383
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
#define EBADF    9
#define EINVAL  22

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
 * sys_stat/sys_lstat/sys_newfstatat below (all take a path; sys_fstat
 * looks up an already-open fd instead, a genuinely different kind of
 * lookup, kept separate). Returns 0 on success, -ENOENT if nothing by
 * that name exists. The lookup order itself is syscall_common.h's own
 * shared stat_lookup() (checkpoint 21) -- only fill_stat's byte
 * offsets are genuinely i386-specific. */
static int stat_path(const char *ramfs_path, unsigned char *sb) {
	return stat_lookup(ramfs_path, fill_stat, sb);
}

/* arg0=path, arg1=statbuf -- SYS_stat (stat64), i386's own preferred
 * real syscall for a plain absolute/cwd-relative stat() (see this
 * file's own header comment on why i386 needs this at all, unlike
 * riscv64). Real bug this cwd resolution fixes, found running real
 * busybox ash's own `ls` for the first time: this used to skip it
 * entirely ("every real caller passes an absolute path"), true of
 * every one-shot P4/P5 test but not of real coreutils -- `ls` calls
 * stat(".") directly, no fstatat/dirfd involved at all, and "." with
 * no cwd resolution isn't a path this ramfs has any entry for. Uses
 * syscall_common.h's shared resolve_user_path() (checkpoint 21) --
 * used to be a private resolve_i386_path() here, byte-for-byte the
 * same logic as syscall_posix.c's own. */
static void sys_stat(struct regs *r) {
	char path[PATH_MAX_LOCAL];
	unsigned char *sb = (unsigned char *)sys_arg(r, 1);
	paging_ensure_writable((unsigned long)sb, ST_STRUCT_SIZE);
	resolve_user_path(path, (const char *)sys_arg(r, 0));
	int ret = stat_path(path, sb);
	sys_ret(r, ret < 0 ? (unsigned long)ret : 0);
}

/* arg0=path, arg1=statbuf -- SYS_lstat (lstat64), musl-i386's own
 * preferred real syscall for a plain (non-fd-relative) lstat(), same
 * "legacy single-number form i386 still has" reasoning as sys_stat.
 * This ramfs has no real symlink *targets* to resolve differently from
 * a plain stat() (mm/ramfs.h's own dir-entry synthesis just marks a
 * name DT_LNK for display -- see syscall_posix.c's own
 * sys_getdents64()), so lstat() and stat() are honestly the same
 * lookup here. */
static void sys_lstat(struct regs *r) {
	sys_stat(r);
}

/* arg0=fd, arg1=offset_high, arg2=offset_low, arg3=result (a real
 * user pointer to an 8-byte off_t, written on success), arg4=whence
 * -- SYS__llseek, real bug found running real self-hosted TCC for the
 * first time (checkpoint 19's own closure test): musl-i386's own
 * src/unistd/lseek.c *always* prefers this over plain SYS_lseek
 * whenever it's defined for the target arch, which it is for i386 (a
 * legacy 32-bit-off_t-vs-real-64-bit-off_t compatibility syscall, not
 * something riscv64 -- 64-bit off_t natively, no such split -- has any
 * equivalent of), so sys_lseek (syscall_posix.c, shared, and correct
 * on its own terms) was simply never reached by anything real. Not
 * shared with syscall_posix.c's own sys_lseek: the argument shape
 * (offset split across two registers, the result handed back through
 * a pointer instead of the return register) is different enough that
 * sharing would mean *this* function reassembling the split offset
 * and re-deriving syscall_posix.c's return convention right back out
 * again -- there isn't a meaningful "generic body" left once that's
 * accounted for, just this. */
static void sys_llseek(struct regs *r) {
	unsigned long fd = sys_arg(r, 0);
	unsigned int offset_high = (unsigned int)sys_arg(r, 1);
	unsigned int offset_low = (unsigned int)sys_arg(r, 2);
	unsigned long long *result = (unsigned long long *)sys_arg(r, 3);
	unsigned long whence = sys_arg(r, 4);

	struct fd_entry *entry = fd >= 3 ? process_fd_get((int)fd - 3) : 0;
	if (!entry) {
		sys_ret(r, (unsigned long)-EBADF);
		return;
	}
	unsigned long long size = entry->dynfile ? entry->dynfile->size : entry->size;
	long long offset = ((long long)offset_high << 32) | (long long)offset_low;
	long long new_pos;
	switch (whence) {
		case 0: new_pos = offset; break;                          /* SEEK_SET */
		case 1: new_pos = (long long)entry->pos + offset; break;  /* SEEK_CUR */
		case 2: new_pos = (long long)size + offset; break;        /* SEEK_END */
		default: sys_ret(r, (unsigned long)-EINVAL); return;
	}
	if (new_pos < 0) {
		sys_ret(r, (unsigned long)-EINVAL);
		return;
	}
	entry->pos = (unsigned long)new_pos;
	paging_ensure_writable((unsigned long)result, sizeof(*result));
	*result = (unsigned long long)new_pos;
	sys_ret(r, 0);
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
	const char *user_path = (const char *)sys_arg(r, 1);
	unsigned char *sb = (unsigned char *)sys_arg(r, 2);
	paging_ensure_writable((unsigned long)sb, ST_STRUCT_SIZE);

	const char *base = process_current_cwd();
	long dirfd = (long)sys_arg(r, 0);
	if (user_path[0] != '/' && dirfd != -100 /* AT_FDCWD */) {
		struct fd_entry *df = dirfd >= 3 ? process_fd_get((int)dirfd - 3) : 0;
		if (!df || !df->is_dir) { sys_ret(r, (unsigned long)-9 /* EBADF */); return; }
		base = df->path;
	}
	char input[PATH_MAX_LOCAL], resolved[PATH_MAX_LOCAL];
	copy_path_from_user(input, user_path);
	resolve_path(resolved, input, base);
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
	 * The value musl reads back is garbage as a result. Since every
	 * real caller only ever gets handed slot 6 anyway (checkpoint 19's
	 * own struct process.tls_base comment: only one slot is real, per
	 * process now, not globally), the simplest correct fix is to just
	 * not require entry_number to be meaningful -- every real call
	 * only cares that base_addr is right, which it is. */
	int slot = 6;
	gdt_set_tls_entry(slot, base_addr);
	desc[0] = (unsigned int)slot; /* kernel writes the allocated slot back */

	/* checkpoint 19: remember it against *this* process -- see struct
	 * process's own tls_base comment for the real bug this fixes.
	 * process_get_current() is 0 for the original one-shot P4/P5
	 * ring3 test (predates process_init() entirely, same as every
	 * other process_get_current()-checking call site in this kernel),
	 * which never needed more than one live TLS user to begin with. */
	struct process *p = process_get_current();
	if (p)
		p->tls_base = base_addr;

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
	case SYS_nanosleep:                            sys_nanosleep(r); return;
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
	case SYS__llseek:                                                                                 sys_llseek(r); return;
	case SYS_unlink:                                                                                    sys_unlink(r); return;
	case SYS_execve:                                                                                       sys_execve(r); return;
	case SYS_getcwd:                                                                                          sys_getcwd(r); return;
	case SYS_chdir:                                                                                             sys_chdir(r); return;
	case SYS_stat:                                                                                                 sys_stat(r); return;
	case SYS_lstat:                                                                                                sys_lstat(r); return;
	case SYS_fstat:                                                                                                   sys_fstat(r); return;
	case SYS_newfstatat:                                                                                              sys_newfstatat(r); return;
	case SYS_getdents64:                                                                                                 sys_getdents64(r); return;
	case SYS_fcntl:                                                                                                         sys_fcntl(r); return;
	case SYS_dup2:                                                                                                             sys_dup2(r); return;
	case SYS_dup3:                                                                                                                sys_dup3(r); return;
	case SYS_pipe2:                                                                                                               sys_pipe2(r); return;
	case SYS_renameat2:                                                                                                           sys_renameat2(r); return;
	case SYS_access:                                                                                                                 sys_access(r); return;
	case SYS_faccessat:                                                                                                                 sys_faccessat(r); return;
	/* Real bug, found running real busybox ash for the first time:
	 * musl's own src/stat/fstatat.c always tries SYS_statx *first*
	 * (unconditionally on every real stat()/fstat()/fstatat() call --
	 * confirmed by reading it, this file's own header comment already
	 * covers why), and only falls back to SYS_stat/SYS_fstat/
	 * SYS_newfstatat (all implemented above, and already proven
	 * correct on their own) once statx itself reports -ENOSYS. Every
	 * ash PATH lookup calls this at least once, so leaving it to fall
	 * through to the loud default case below would print "FATAL:" on
	 * every single command typed -- alarming, and would fail this
	 * kernel's own --must-not-contain "FATAL" test convention, for a
	 * syscall whose *correct* answer really is "not implemented, try
	 * something else" (implementing statx for real would mean a whole
	 * second struct-stat-shaped layout, `struct statx`, for something
	 * every real caller here already gets an equally correct answer
	 * from one syscall later). */
	case SYS_statx:
		r->eax = (unsigned int)-ENOSYS;
		return;
	default:
		kprintf("FATAL: unimplemented syscall %u\n", r->eax);
		r->eax = (unsigned int)-ENOSYS;
	}
}

void syscall_init(void) {
	syscall_set_handler(syscall_dispatch);
	kprintf("syscall: dispatch installed (write, fork, open, "
		"execve, stat/fstat/newfstatat, dup2/dup3, access/faccessat, "
		"clone, wait4, pipe2, nanosleep, getdents64, and more -- see this file's own dispatch)\n");
}
