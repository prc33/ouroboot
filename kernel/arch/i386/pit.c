/* 8254 PIT, channel 0, configured for a periodic IRQ0 at the requested
 * frequency. This is the preemption clock for P4's scheduler. */
#include "kernel.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE_HZ  1193182u

static inline void outb(unsigned short port, unsigned char val) {
	__asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void pit_init(unsigned int hz) {
	unsigned int divisor = PIT_BASE_HZ / hz;
	outb(PIT_COMMAND, 0x36); /* channel 0, lo/hi byte, mode 3 (square wave), binary */
	outb(PIT_CHANNEL0, divisor & 0xFF);
	outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);
	kprintf("pit: channel 0 at %u Hz (divisor=%u)\n", hz, divisor);
}
