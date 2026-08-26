/* int $0x80 entry point. Kept separate from the generated exception/
 * IRQ stubs in isr_stubs.S: this is the one gate in the whole IDT that
 * needs DPL=3 (user-callable) rather than DPL=0 -- see idt.c's
 * idt_set_user_gate. Otherwise identical shape to the generated
 * stubs, and reuses the same isr_common_stub tail. */
.globl isr128
isr128:
	cli
	push $0
	push $128
	jmp isr_common_stub
