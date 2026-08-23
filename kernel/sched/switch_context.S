/* void switch_context(unsigned int *old_esp_store, unsigned int new_esp)
 *
 * Saves the four callee-saved GP registers and the current esp into
 * *old_esp_store, switches esp to new_esp, restores the other task's
 * four registers from there, and `ret`s -- which lands wherever that
 * stack's saved return address points: either back into a previous
 * switch_context call site inside that task (normal resume), or at a
 * task's entry trampoline the very first time it runs (see
 * task_init's hand-built initial stack in sched/task.c). */
.globl switch_context
switch_context:
	movl 4(%esp), %eax   /* old_esp_store */
	movl 8(%esp), %edx   /* new_esp */

	push %ebx
	push %esi
	push %edi
	push %ebp

	movl %esp, (%eax)
	movl %edx, %esp

	pop %ebp
	pop %edi
	pop %esi
	pop %ebx

	ret
