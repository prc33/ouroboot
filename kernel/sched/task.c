#include "kernel.h"
#include "task.h"

/* Fixed at 2 for this checkpoint -- "two hardcoded tasks" is the P4
 * exit criterion in the plan. Generalizing to N runnable tasks is a
 * P5+ concern once there's a real process table driving it. */
#define NUM_TASKS 2

static struct task *tasks[NUM_TASKS];
static int current_task = -1;

void task_init(struct task *t, int id, void (*entry)(void)) {
	t->id = id;

	/* Build a stack that looks exactly like one switch_context call
	 * already happened on it: four fake callee-saved registers, then
	 * a return address pointing straight at `entry`. When this task
	 * is switched to for the first time, switch_context's four pops
	 * consume the fakes and `ret` jumps into entry() directly -- no
	 * iret involved, which is why every task entry function's first
	 * action must be `sti` (see task_a/task_b in kmain.c): the normal
	 * "interrupts get re-enabled" step (irq_common_stub's sti before
	 * iret) is skipped entirely on a task's very first launch. */
	unsigned int *top = &t->stack[TASK_STACK_WORDS];
	top -= 1; top[0] = (unsigned int)(unsigned long)entry; /* return address */
	unsigned int *frame = top - 4;
	frame[0] = 0; /* fake ebp */
	frame[1] = 0; /* fake edi */
	frame[2] = 0; /* fake esi */
	frame[3] = 0; /* fake ebx */
	t->esp = (unsigned int)(unsigned long)frame;
}

/* Called once from kmain to hand control to task[0] for the first
 * time. kmain's own stack is never returned to -- see kmain.c for how
 * the test concludes without needing to unwind back here. */
void task_register(int slot, struct task *t) {
	tasks[slot] = t;
}

void task_start_scheduler(struct task *first) {
	static unsigned int discard_esp;
	current_task = 0;
	switch_context(&discard_esp, first->esp);
}

void task_yield(void) {
	int next = (current_task + 1) % NUM_TASKS;
	struct task *old = tasks[current_task];
	struct task *new = tasks[next];
	current_task = next;
	switch_context(&old->esp, new->esp);
}
