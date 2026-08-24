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

struct process {
	int pid;
	int ppid;                          /* checkpoint 7: real parent, for process_wait4()'s "is this my child" check */
	enum process_state state;
	unsigned long *root_table;         /* this process's own address space (mm/paging.h) */
	unsigned long kernel_sp;           /* switch_context()-managed, see sched/riscv64_switch_context.S */
	unsigned long kernel_stack[PROC_KSTACK_WORDS];
	struct regs user_regs;             /* saved U-mode context when this process isn't the one currently running */
	int exit_code;
};

void process_init(void);

/* Creates a process from a real ELF already sitting in memory (same
 * "embedded as static data, no filesystem yet" simplification as
 * mm/elf.c/riscv64_kmain.c's run_elf_test) in its own fresh address
 * space, with argv = {arg0, NULL}. Returns 0 on failure (table full,
 * out of memory, bad ELF). */
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

/* Runs once, the next time the process table completely drains (no
 * RUNNABLE process left) -- riscv64_kmain.c uses this to chain into
 * the next checkpoint's own test in sequence, see
 * sched/riscv64_process.c's own comment. */
void process_set_drain_hook(void (*hook)(void));

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

/* checkpoint 7: real wait4(pid, status, ...), restricted to what this
 * checkpoint's test/init actually need: `pid` a specific child's real
 * pid, or -1 for "any child". Blocks (cooperatively -- see this
 * function's own comment in sched/riscv64_process.c) until a matching
 * child becomes a zombie, then reaps it (frees its process-table
 * slot) and returns its pid, with *status_out set to the same
 * WIFEXITED/WEXITSTATUS-decodable encoding real Linux uses. */
long process_wait4(int pid, int *status_out);

#endif
