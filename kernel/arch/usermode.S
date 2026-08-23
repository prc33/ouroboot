/* void enter_usermode(unsigned int entry, unsigned int user_esp)
 *
 * Never returns (in the sense that it doesn't return to its caller --
 * execution continues in ring3 at `entry`). Builds the same stack
 * shape the CPU would build automatically on a real interrupt-from-
 * ring3, by hand, since there's no actual interrupt happening the
 * first time a task is launched into userspace -- iret just doesn't
 * care how its frame got there. */
.globl enter_usermode
enter_usermode:
	movl 4(%esp), %eax   /* entry point */
	movl 8(%esp), %edx   /* user stack top */

	movl $0x23, %ecx     /* user data selector: GDT index 4, RPL 3 */
	movw %cx, %ds
	movw %cx, %es
	movw %cx, %fs
	movw %cx, %gs

	pushl $0x23           /* ss */
	pushl %edx             /* esp */
	pushfl
	popl %ecx
	orl $0x200, %ecx       /* set IF: interrupts stay enabled in user mode */
	pushl %ecx             /* eflags */
	pushl $0x1B            /* cs: GDT index 3, RPL 3 */
	pushl %eax              /* eip */
	iret
