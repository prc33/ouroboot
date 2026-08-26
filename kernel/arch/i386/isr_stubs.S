/* GENERATED FILE -- see arch/gen_isr_stubs.py. Do not hand-edit. */
.globl isr_common_stub
.globl irq_common_stub

.globl isr0
isr0:
	cli
	push $0
	push $0
	jmp isr_common_stub

.globl isr1
isr1:
	cli
	push $0
	push $1
	jmp isr_common_stub

.globl isr2
isr2:
	cli
	push $0
	push $2
	jmp isr_common_stub

.globl isr3
isr3:
	cli
	push $0
	push $3
	jmp isr_common_stub

.globl isr4
isr4:
	cli
	push $0
	push $4
	jmp isr_common_stub

.globl isr5
isr5:
	cli
	push $0
	push $5
	jmp isr_common_stub

.globl isr6
isr6:
	cli
	push $0
	push $6
	jmp isr_common_stub

.globl isr7
isr7:
	cli
	push $0
	push $7
	jmp isr_common_stub

.globl isr8
isr8:
	cli
	push $8
	jmp isr_common_stub

.globl isr9
isr9:
	cli
	push $0
	push $9
	jmp isr_common_stub

.globl isr10
isr10:
	cli
	push $10
	jmp isr_common_stub

.globl isr11
isr11:
	cli
	push $11
	jmp isr_common_stub

.globl isr12
isr12:
	cli
	push $12
	jmp isr_common_stub

.globl isr13
isr13:
	cli
	push $13
	jmp isr_common_stub

.globl isr14
isr14:
	cli
	push $14
	jmp isr_common_stub

.globl isr15
isr15:
	cli
	push $0
	push $15
	jmp isr_common_stub

.globl isr16
isr16:
	cli
	push $0
	push $16
	jmp isr_common_stub

.globl isr17
isr17:
	cli
	push $17
	jmp isr_common_stub

.globl isr18
isr18:
	cli
	push $0
	push $18
	jmp isr_common_stub

.globl isr19
isr19:
	cli
	push $0
	push $19
	jmp isr_common_stub

.globl isr20
isr20:
	cli
	push $0
	push $20
	jmp isr_common_stub

.globl isr21
isr21:
	cli
	push $0
	push $21
	jmp isr_common_stub

.globl isr22
isr22:
	cli
	push $0
	push $22
	jmp isr_common_stub

.globl isr23
isr23:
	cli
	push $0
	push $23
	jmp isr_common_stub

.globl isr24
isr24:
	cli
	push $0
	push $24
	jmp isr_common_stub

.globl isr25
isr25:
	cli
	push $0
	push $25
	jmp isr_common_stub

.globl isr26
isr26:
	cli
	push $0
	push $26
	jmp isr_common_stub

.globl isr27
isr27:
	cli
	push $0
	push $27
	jmp isr_common_stub

.globl isr28
isr28:
	cli
	push $0
	push $28
	jmp isr_common_stub

.globl isr29
isr29:
	cli
	push $0
	push $29
	jmp isr_common_stub

.globl isr30
isr30:
	cli
	push $0
	push $30
	jmp isr_common_stub

.globl isr31
isr31:
	cli
	push $0
	push $31
	jmp isr_common_stub

.globl irq0
irq0:
	cli
	push $0
	push $32
	jmp irq_common_stub

.globl irq1
irq1:
	cli
	push $0
	push $33
	jmp irq_common_stub

.globl irq2
irq2:
	cli
	push $0
	push $34
	jmp irq_common_stub

.globl irq3
irq3:
	cli
	push $0
	push $35
	jmp irq_common_stub

.globl irq4
irq4:
	cli
	push $0
	push $36
	jmp irq_common_stub

.globl irq5
irq5:
	cli
	push $0
	push $37
	jmp irq_common_stub

.globl irq6
irq6:
	cli
	push $0
	push $38
	jmp irq_common_stub

.globl irq7
irq7:
	cli
	push $0
	push $39
	jmp irq_common_stub

.globl irq8
irq8:
	cli
	push $0
	push $40
	jmp irq_common_stub

.globl irq9
irq9:
	cli
	push $0
	push $41
	jmp irq_common_stub

.globl irq10
irq10:
	cli
	push $0
	push $42
	jmp irq_common_stub

.globl irq11
irq11:
	cli
	push $0
	push $43
	jmp irq_common_stub

.globl irq12
irq12:
	cli
	push $0
	push $44
	jmp irq_common_stub

.globl irq13
irq13:
	cli
	push $0
	push $45
	jmp irq_common_stub

.globl irq14
irq14:
	cli
	push $0
	push $46
	jmp irq_common_stub

.globl irq15
irq15:
	cli
	push $0
	push $47
	jmp irq_common_stub

isr_common_stub:
	pusha
	movw %gs, %ax
	push %eax
	movw %fs, %ax
	push %eax
	movw %es, %ax
	push %eax
	movw %ds, %ax
	push %eax
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %fs
	movw %ax, %gs
	push %esp
	call isr_handler
	add $4, %esp
	pop %eax
	movw %ax, %ds
	pop %eax
	movw %ax, %es
	pop %eax
	movw %ax, %fs
	pop %eax
	movw %ax, %gs
	popa
	add $8, %esp
	sti
	iret

irq_common_stub:
	pusha
	movw %gs, %ax
	push %eax
	movw %fs, %ax
	push %eax
	movw %es, %ax
	push %eax
	movw %ds, %ax
	push %eax
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %fs
	movw %ax, %gs
	push %esp
	call irq_handler
	add $4, %esp
	pop %eax
	movw %ax, %ds
	pop %eax
	movw %ax, %es
	pop %eax
	movw %ax, %fs
	pop %eax
	movw %ax, %gs
	popa
	add $8, %esp
	sti
	iret
