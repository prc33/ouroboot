#include "kernel.h"
#include "idt.h"

struct idt_entry {
	unsigned short base_low;
	unsigned short sel;
	unsigned char always0;
	unsigned char flags;
	unsigned short base_high;
} __attribute__((packed));

struct idt_ptr {
	unsigned short limit;
	unsigned int base;
} __attribute__((packed));

#define IDT_ENTRIES 256
static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr ip;

static void (*isr_handlers[32])(struct regs *);
static void (*irq_handlers[16])(struct regs *);
static void (*syscall_handler)(struct regs *);

/* declared in isr_stubs.S, one label per vector */
extern void isr0(void), isr1(void), isr2(void), isr3(void), isr4(void),
	isr5(void), isr6(void), isr7(void), isr8(void), isr9(void),
	isr10(void), isr11(void), isr12(void), isr13(void), isr14(void),
	isr15(void), isr16(void), isr17(void), isr18(void), isr19(void),
	isr20(void), isr21(void), isr22(void), isr23(void), isr24(void),
	isr25(void), isr26(void), isr27(void), isr28(void), isr29(void),
	isr30(void), isr31(void);
extern void irq0(void), irq1(void), irq2(void), irq3(void), irq4(void),
	irq5(void), irq6(void), irq7(void), irq8(void), irq9(void),
	irq10(void), irq11(void), irq12(void), irq13(void), irq14(void),
	irq15(void);
extern void isr128(void); /* arch/syscall_stub.S -- the one user-callable (DPL=3) gate */

static void idt_set_gate(int n, unsigned int base, unsigned short sel, unsigned char flags) {
	idt[n].base_low = base & 0xFFFF;
	idt[n].base_high = (base >> 16) & 0xFFFF;
	idt[n].sel = sel;
	idt[n].always0 = 0;
	idt[n].flags = flags;
}

static inline void idt_flush(struct idt_ptr *p) {
	__asm__ volatile ("lidt (%0)" : : "r"(p));
}

static const char *exception_names[32] = {
	"Divide-by-zero", "Debug", "NMI", "Breakpoint",
	"Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
	"Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
	"Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
	"x87 FP Exception", "Alignment Check", "Machine Check", "SIMD FP Exception",
	"Virtualization", "Control Protection", "Reserved", "Reserved",
	"Reserved", "Reserved", "Reserved", "Reserved",
	"Reserved", "Reserved", "Reserved", "Reserved",
};

/* Called from arch/isr_stubs.S:isr_common_stub for every CPU exception
 * -- and also, via arch/syscall_stub.S:isr128, for int $0x80. Vector
 * 128 is neither an exception (0-31) nor a remapped IRQ (32-47), so
 * it's special-cased here rather than fitting either dispatch table. */
void isr_handler(struct regs *r) {
	if (r->int_no == 128) {
		if (syscall_handler)
			syscall_handler(r);
		else
			kprintf("FATAL: int 0x80 with no syscall handler registered\n");
		return;
	}
	if (r->int_no < 32 && isr_handlers[r->int_no]) {
		isr_handlers[r->int_no](r);
		return;
	}
	kprintf("\n!! UNHANDLED EXCEPTION %u: %s (err=%x) eip=%x\n",
		r->int_no, exception_names[r->int_no & 31], r->err_code, r->eip);
	kprintf("FATAL: unhandled exception, halting\n");
	for (;;) __asm__ volatile ("cli\n hlt");
}

/* Called from arch/isr_stubs.S:irq_common_stub for every PIC IRQ. PIC
 * EOI is sent here (by pic_send_eoi in the driver), not by hand-written
 * per-stub logic, so every IRQ handler gets it uniformly. */
void irq_handler(struct regs *r) {
	extern void pic_send_eoi(unsigned char irq);
	int irq_no = r->int_no - 32;
	if (irq_no >= 0 && irq_no < 16 && irq_handlers[irq_no])
		irq_handlers[irq_no](r);
	pic_send_eoi((unsigned char)irq_no);
}

void isr_register_handler(int n, void (*handler)(struct regs *)) {
	if (n >= 0 && n < 32)
		isr_handlers[n] = handler;
}

void irq_register_handler(int n, void (*handler)(struct regs *)) {
	if (n >= 0 && n < 16)
		irq_handlers[n] = handler;
}

void syscall_set_handler(void (*handler)(struct regs *)) {
	syscall_handler = handler;
}

void idt_init(void) {
	ip.limit = sizeof(idt) - 1;
	ip.base = (unsigned int)(unsigned long)&idt;

	for (int i = 0; i < IDT_ENTRIES; i++)
		idt_set_gate(i, 0, 0, 0);

	void (*isr_table[32])(void) = {
		isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
		isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
		isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
		isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
	};
	for (int i = 0; i < 32; i++)
		idt_set_gate(i, (unsigned int)(unsigned long)isr_table[i], 0x08, 0x8E);

	void (*irq_table[16])(void) = {
		irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7,
		irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15,
	};
	for (int i = 0; i < 16; i++)
		idt_set_gate(32 + i, (unsigned int)(unsigned long)irq_table[i], 0x08, 0x8E);

	/* 0xEE vs the 0x8E used everywhere else: DPL=3 instead of DPL=0,
	 * i.e. software in ring3 is actually permitted to execute
	 * `int $0x80` at all. Every other gate is deliberately kernel-only
	 * -- ring3 code hitting, say, a divide-by-zero still correctly
	 * takes the fault (the CPU delivers exceptions regardless of gate
	 * DPL), but it can't *invoke* isr3 or isr14 directly by hand. */
	idt_set_gate(0x80, (unsigned int)(unsigned long)isr128, 0x08, 0xEE);

	idt_flush(&ip);
	kprintf("idt: loaded (256 entries, 32 exceptions + 16 IRQs + int 0x80 wired)\n");
}
