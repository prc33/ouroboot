#ifndef TASK_H
#define TASK_H

#define TASK_STACK_WORDS 1024 /* 4KB per task */

struct task {
	unsigned int esp;
	unsigned int stack[TASK_STACK_WORDS];
	int id;
};

void task_init(struct task *t, int id, void (*entry)(void));
void task_register(int slot, struct task *t);
void task_start_scheduler(struct task *first);
void task_yield(void);

/* implemented in sched/switch_context.S */
void switch_context(unsigned int *old_esp_store, unsigned int new_esp);

#endif
