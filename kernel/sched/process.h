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

#endif
