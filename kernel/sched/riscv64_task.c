/* Same 2-task cooperative round-robin scheduler as sched/task.c --
 * see that file for the full rationale (fixed at 2 tasks for this
 * checkpoint, generalizing to N is a later concern). Only the
 * register set in the hand-built initial stack differs: riscv64's
 * calling convention has 13 callee-saved registers (ra + s0-s11) vs
 * i386's 4, matching sched/riscv64_switch_context.S's save/restore
 * list exactly. */
#include "kernel.h"
#include "task.h"

#define NUM_TASKS 2

static struct task *tasks[NUM_TASKS];
static int current_task = -1;

void task_init(struct task *t, int id, void (*entry)(void)) {
	t->id = id;

	/* Build a stack that looks exactly like one switch_context call
	 * already happened on it: 13 fake callee-saved registers (ra
	 * pointing straight at `entry`, s0-s11 all zero -- never read
	 * before entry() runs its own thing), plus 8 bytes padding to
	 * keep the frame a 16-byte-aligned 112 bytes, matching
	 * sched/riscv64_switch_context.S's `addi sp,sp,-112` exactly.
	 * When this task is switched to for the first time, switch_context's
	 * restore sequence loads these straight into the real registers
	 * and `jalr x0,0(ra)` jumps into entry() directly -- no sret
	 * involved, which is why every task entry function's first action
	 * must re-enable whatever this kernel needs re-enabled per task
	 * (see riscv64_kmain.c's task_a/task_b, mirroring i386's kmain.c
	 * comment about the same thing). */
	unsigned long *top = &t->stack[TASK_STACK_WORDS];
	unsigned long *frame = top - 14; /* 14 words = 112 bytes */
	frame[0] = (unsigned long)entry; /* ra */
	for (int i = 1; i < 13; i++)
		frame[i] = 0; /* s0..s11 */
	t->sp = (unsigned long)frame;
}

void task_register(int slot, struct task *t) {
	tasks[slot] = t;
}

void task_start_scheduler(struct task *first) {
	static unsigned long discard_sp;
	current_task = 0;
	switch_context(&discard_sp, first->sp);
}

void task_yield(void) {
	int next = (current_task + 1) % NUM_TASKS;
	struct task *old = tasks[current_task];
	struct task *new = tasks[next];
	current_task = next;
	switch_context(&old->sp, new->sp);
}
