#ifndef PROCESS_H
#define PROCESS_H

#include "arch/riscv64_trap.h"

/* General process table -- checkpoint 6 (see sched/riscv64_process.c
 * for the full design). Distinct from sched/task.h's `struct task`:
 * that's the fixed-2, kernel-mode-only cooperative demo from P4,
 * never connected to U-mode/syscalls at all. This is real processes:
 * their own address space, their own kernel stack, a saved U-mode
 * trapframe, real state (RUNNABLE/ZOMBIE). */

#define PROC_KSTACK_WORDS 1024 /* 8KB kernel stack per process, words are 8 bytes */
#define MAX_PROCESSES 8

enum process_state {
	PROC_UNUSED = 0,
	PROC_RUNNABLE,
	PROC_ZOMBIE,
};

struct ramfs_dynamic_file; /* mm/ramfs.h -- forward-declared rather than
                             * #included, process.h doesn't otherwise need it */

/* checkpoint 8: per-process file descriptors, fd numbers 3.. (0/1/2
 * are the UART console, handled specially in arch/riscv64_syscall.c's
 * sys_read/sys_write same as always -- never stored here). Two shapes,
 * distinguished by `dynfile`: an open mm/ramfs.h *fixed* file (read-only,
 * `data`+`size` point straight at that file's embedded bytes, `pos` is
 * this fd's own cursor into it -- the original, checkpoint-8 shape), or
 * (checkpoint 12) an open *dynamic* (real, writable, created-at-runtime)
 * file, `dynfile` non-NULL and `data`/`size` unused -- see
 * mm/ramfs.h's own struct ramfs_dynamic_file and
 * arch/riscv64_syscall.c's sys_openat/sys_write/sys_read. */
#define MAX_FDS 32

struct fd_entry {
	int used;
	const unsigned char *data;
	unsigned long size;
	unsigned long pos;
	int is_dir; /* for directories, pos is the next readdir entry */
	struct ramfs_dynamic_file *dynfile; /* checkpoint 12 -- see this struct's own comment above */
	char path[128]; /* normalized directory path when is_dir is set */
};

struct process {
	int pid;
	int ppid;                          /* checkpoint 7: real parent, for process_wait4()'s "is this my child" check */
	enum process_state state;
	unsigned long *root_table;         /* this process's own address space (mm/paging.h) */
	unsigned long user_stack_lo;       /* lowest currently mapped stack page */
	unsigned long user_stack_hi;       /* fixed top of the downward-growing stack */
	unsigned long user_stack_limit;    /* lowest address to which it may grow */
	unsigned long user_brk;
	unsigned long user_mmap_next;
	unsigned long kernel_sp;           /* switch_context()-managed, see sched/riscv64_switch_context.S */
	unsigned long kernel_stack[PROC_KSTACK_WORDS];
	struct regs user_regs;             /* saved U-mode context when this process isn't the one currently running */
	int exit_code;
	struct fd_entry fds[MAX_FDS];      /* checkpoint 8 -- see the struct's own comment */
	/* checkpoint 12: real shell I/O redirection (`echo x > file`) needs
	 * real dup2()/dup3() -- ash forks, opens the target file, dup3()s
	 * it onto fd 1, closes the original fd, *then* runs the command,
	 * which just writes to fd 1 as always. fds[0..2] don't exist (0/1/2
	 * are permanently the console, a hardcoded special case in
	 * arch/riscv64_syscall.c's sys_read/sys_write/sys_close, never
	 * stored in fds[] -- this struct's own comment), so dup3()ing onto
	 * one of them needs somewhere else to record "fd N is temporarily
	 * not the console". stdio_override[N].used gates it: sys_read/
	 * sys_write/sys_close all check this array first for fd 0/1/2,
	 * falling back to the ordinary console behavior when a slot isn't
	 * in use. Simplification, not full dup2 semantics: this *copies*
	 * the source fd's state rather than making the two fds genuinely
	 * share one open-file position -- correct for the dominant case
	 * (open, dup, close-the-original, never touch both concurrently
	 * again), diverges from real POSIX only if a caller keeps both
	 * fds open and interleaves reads/writes through both, which
	 * nothing this kernel runs does. */
	struct fd_entry stdio_override[3];
	char cwd[128];
};

void process_init(void);

/* Creates a process from a real ELF (an initrd-loaded ramfs file, or
 * kernel/test/riscv64_checkpoints.c's own one-shot payloads) in its
 * own fresh address space, with the given real argv (NUL-terminated
 * strings, all already kernel-resident -- see
 * sched/riscv64_process.c's build_user_stack() for why). Returns 0 on
 * failure (table full, out of memory, bad ELF). */
struct process *process_create_from_elf_argv(const unsigned char *elf_data, unsigned long elf_size, char *const argv[], int argc);

/* Single-arg0 convenience wrapper (argv = {arg0, NULL}) -- see
 * sched/riscv64_process.c's own comment. */
struct process *process_create_from_elf(const unsigned char *elf_data, unsigned long elf_size, const char *arg0);

/* Cooperative round-robin over every RUNNABLE process, same technique
 * as sched/riscv64_task.c's task_yield -- switch_context() to the
 * next one. Called both to kick off the very first process and, via
 * SYS_sched_yield, from inside a syscall to hand off to the next
 * process without exiting. Returns (i.e. is itself a coroutine yield
 * point, not a one-way jump) once this process is picked to run
 * again -- exactly like switch_context() always has. */
void process_schedule(void);

/* Marks the *currently running* process ZOMBIE and reschedules --
 * never returns. arch/riscv64_syscall.c's sys_exit_group calls this
 * once the general process table is in play (see that file's
 * comment for how it tells the two eras apart). */
void process_exit_current(int exit_code) __attribute__((noreturn));

/* Starts the scheduler running `first` -- the process-table
 * equivalent of sched/task.h's task_start_scheduler. Also flips
 * process_mode_active() on, which is how arch/riscv64_syscall.c's
 * sys_exit_group tells the old P4/P5 one-shot exit chain apart from a
 * real process's exit (see that file's comment). */
void process_run(struct process *first);

/* Whether process_run() has been called yet -- see its own comment. */
int process_mode_active(void);

/* The real pid of whichever process is currently running -- 0 if
 * process_mode_active() is false (no process context exists yet).
 * arch/riscv64_syscall.c uses this for getpid()/gettid() (this kernel
 * has no real threads, so they're the same value -- correct, not a
 * simplification: that's true on real Linux too for a single-threaded
 * process) instead of the pre-checkpoint-6 hardcoded "tid 1" every
 * process used to get regardless of which one it actually was. */
int process_current_pid(void);

/* The real parent pid of whichever process is currently running -- 0
 * if there isn't one (created directly by kmain, not fork()) or no
 * process context exists yet. arch/riscv64_syscall.c's sys_getppid. */
int process_current_ppid(void);
unsigned long process_current_brk(void);
void process_set_current_brk(unsigned long value);
unsigned long process_take_mmap(unsigned long length);
int process_handle_stack_fault(unsigned long address);
const char *process_current_cwd(void);
void process_set_current_cwd(const char *path);

/* Runs once, the next time the process table completely drains (no
 * RUNNABLE process left) -- kernel/test/riscv64_checkpoints.c uses
 * this to chain each checkpoint into the next one in sequence, see
 * sched/riscv64_process.c's own comment. */
void process_set_drain_hook(void (*hook)(void));

/* The ordinary, checkpoint-agnostic "nothing left to schedule" halt --
 * every real boot (product included) reaches this the same way, once
 * its own last process exits with no drain hook registered to chain
 * into anything further. Declared here (not static in
 * sched/riscv64_process.c) so kernel/test/riscv64_checkpoints.c's own
 * final stage can call it after printing its own closing message --
 * see that file's finish_checkpoint_boot() and this function's own
 * comment in sched/riscv64_process.c. */
void process_halt(void) __attribute__((noreturn));

/* Fires at most once, the next time a bare SYS_exit or a pre-process-
 * mode SYS_exit_group reaches arch/riscv64_syscall.c -- i.e. before
 * process_init()/process_mode_active() exist at all (P1-P5's one-shot
 * ring3/ELF-loader demos, kernel/test/riscv64_checkpoints.c's own
 * run_ring3_test/run_elf_test). Generic counterpart to
 * process_set_drain_hook() above for that earlier boot phase, so the
 * syscall layer never has to hardcode which checkpoint runs next or
 * print a checkpoint-numbered message itself -- see
 * arch/riscv64_syscall.c's own comment on sys_exit_impl(). A no-op
 * default (plain halt) if nothing ever registers one, which is every
 * real boot: the product path calls process_init() before any process
 * can exit, so it never reaches this hook at all. */
void syscall_set_pre_process_exit_hook(void (*hook)(void));

/* checkpoint 7: real fork(), via SYS_clone (arch/riscv64_syscall.c --
 * riscv64 has no separate SYS_fork; musl's fork() itself calls
 * SYS_clone(SIGCHLD, 0, ...), confirmed via a real strace, same
 * methodology as every other syscall in this kernel). Clones the
 * *calling* process (current_process) -- COW address space
 * (mm/paging.h's paging_fork_cow) plus a snapshot of `r`, the live
 * trapframe of the ecall that got us here, with a0 forced to 0 (the
 * child's fork() return value; the parent's own a0 -- the child's
 * pid, or -1 -- is set by the syscall handler itself, same as every
 * other syscall's return value). Returns the new child's pid, or -1
 * on failure (process table full / out of memory). */
int process_fork(struct regs *r);

/* checkpoint 7 (blocking)/9 (WNOHANG, -ECHILD) real wait4(pid, status,
 * options, ...): `pid` a specific child's real pid, or -1 for "any
 * child". If `options` has WNOHANG (0x1) set, checks once and returns
 * 0 immediately if no matching child is a zombie *yet* (rather than
 * blocking) -- real shells (confirmed via busybox ash's own source
 * and behavior: it calls wait4() a second time with WNOHANG right
 * after reaping a foreground child, sweeping for anything else
 * already-exited) rely on this to mean "nothing new yet", not "block
 * until something is". Either way, returns -ECHILD immediately if the
 * caller has *no* matching child at all, zombie or not -- omitting
 * this (checkpoint 7's original version didn't check at all) means a
 * WNOHANG sweep with nothing left to reap spins forever indistinguishable
 * from "waiting for a child that will never exist". Otherwise blocks
 * (cooperatively -- see this function's own comment in
 * sched/riscv64_process.c) until a matching child becomes a zombie,
 * then reaps it (frees its process-table slot) and returns its pid,
 * with *status_out set to the same WIFEXITED/WEXITSTATUS-decodable
 * encoding real Linux uses. */
long process_wait4(int pid, int *status_out, int options);

/* checkpoint 8: fd-table access for arch/riscv64_syscall.c's
 * sys_openat/sys_read/sys_close, all against the *currently running*
 * process. All index by the raw fds[] array position (0..MAX_FDS-1)
 * -- the syscall-visible fd number (index + 3, since 0/1/2 are always
 * the UART console, handled separately and never stored here) is
 * entirely the syscall layer's own concern. */
int process_fd_alloc(void);                 /* index of the first unused slot, marked used; -1 if the table's full */
struct fd_entry *process_fd_get(int index); /* 0 if out of range or not currently used */
void process_fd_close(int index);           /* no-op if already unused/out of range */
void process_fd_set(int index, const struct fd_entry *src); /* checkpoint 12: dup3()'s target-by-number half */

/* checkpoint 12: real dup2()/dup3() onto fd 0/1/2 -- see struct
 * process's own stdio_override comment for why these exist. */
struct fd_entry *process_stdio_get(int fd);              /* fd must be 0/1/2; 0 if not currently overridden */
void process_stdio_set(int fd, const struct fd_entry *src); /* fd must be 0/1/2 */
void process_stdio_clear(int fd);                            /* fd must be 0/1/2 */

/* checkpoint 8: real execve(), via SYS_execve. Replaces the
 * *currently running* process's address space with `path` (looked up
 * in mm/ramfs.h) in place -- same pid, same fd table (real execve()
 * semantics: file descriptors survive; only memory and register state
 * don't), brand new address space and entry point. `r` is the live
 * trapframe of the ecall that got us here; on success this rewrites
 * it in place (sepc/sp to the new program's, every GPR else zeroed)
 * so the eventual sret lands directly in the new program -- execve()
 * never "returns" to its caller on success, by definition. Returns
 * < 0 on failure (bad path / out of memory / bad ELF), in which case
 * `r` is left untouched and the syscall handler returns that value
 * as a normal negative-errno result. `argv`/`envp` are real user-space
 * pointer arrays (NUL-terminated char* arrays, each pointing at a
 * NUL-terminated string) -- copied into kernel memory before the old
 * address space is touched, since they live in memory this call is
 * about to replace. */
int process_execve(struct regs *r, const char *path, char **argv, char **envp);

#endif
