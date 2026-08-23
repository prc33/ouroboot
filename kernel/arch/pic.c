/* 8259 PIC remap. By default IRQ0-7 map to interrupt vectors 8-15 and
 * IRQ8-15 map to 0x70-0x77 -- both ranges collide with CPU exception
 * vectors (0-31), so a spurious IRQ would look like a CPU exception.
 * Remap to 32-47, clear of the exception range, before ever enabling
 * interrupts. */
#include "kernel.h"

#define PIC1 0x20
#define PIC1_CMD PIC1
#define PIC1_DATA (PIC1 + 1)
#define PIC2 0xA0
#define PIC2_CMD PIC2
#define PIC2_DATA (PIC2 + 1)

#define PIC_EOI 0x20

static inline void outb(unsigned short port, unsigned char val) {
	__asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline unsigned char inb(unsigned short port) {
	unsigned char ret;
	__asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
	return ret;
}
static inline void io_wait(void) {
	outb(0x80, 0);
}

void pic_remap(void) {
	unsigned char mask1 = inb(PIC1_DATA);
	unsigned char mask2 = inb(PIC2_DATA);

	outb(PIC1_CMD, 0x11); io_wait(); /* ICW1: init, expect ICW4 */
	outb(PIC2_CMD, 0x11); io_wait();
	outb(PIC1_DATA, 0x20); io_wait(); /* ICW2: IRQ0-7 -> vectors 32-39 */
	outb(PIC2_DATA, 0x28); io_wait(); /* ICW2: IRQ8-15 -> vectors 40-47 */
	outb(PIC1_DATA, 0x04); io_wait(); /* ICW3: PIC1 has a slave on IRQ2 */
	outb(PIC2_DATA, 0x02); io_wait(); /* ICW3: slave's identity is 2 */
	outb(PIC1_DATA, 0x01); io_wait(); /* ICW4: 8086 mode */
	outb(PIC2_DATA, 0x01); io_wait();

	outb(PIC1_DATA, mask1); /* restore whatever mask was set before */
	outb(PIC2_DATA, mask2);

	kprintf("pic: remapped, IRQ0-7 -> 32-39, IRQ8-15 -> 40-47\n");
}

void pic_send_eoi(unsigned char irq) {
	if (irq >= 8)
		outb(PIC2_CMD, PIC_EOI);
	outb(PIC1_CMD, PIC_EOI);
}

void pic_set_mask(unsigned char irq) {
	unsigned short port = irq < 8 ? PIC1_DATA : PIC2_DATA;
	unsigned char irqline = irq < 8 ? irq : irq - 8;
	outb(port, inb(port) | (1 << irqline));
}

void pic_clear_mask(unsigned char irq) {
	unsigned short port = irq < 8 ? PIC1_DATA : PIC2_DATA;
	unsigned char irqline = irq < 8 ? irq : irq - 8;
	outb(port, inb(port) & ~(1 << irqline));
}
