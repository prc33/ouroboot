/* Minimal 16550 UART driver, COM1 (0x3F8). Serial console only -- see
 * plan decision D8 (no VGA/8042: the terminal is HTML/xterm.js anyway,
 * reached through this same byte-in/byte-out interface later). */
#include "kernel.h"

#define COM1 0x3F8

static inline void outb(unsigned short port, unsigned char val) {
	__asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline unsigned char inb(unsigned short port) {
	unsigned char ret;
	__asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
	return ret;
}

void serial_init(void) {
	outb(COM1 + 1, 0x00);  /* disable interrupts */
	outb(COM1 + 3, 0x80);  /* enable DLAB (set baud rate divisor) */
	outb(COM1 + 0, 0x03);  /* divisor lo byte: 115200 / 3 = 38400 baud */
	outb(COM1 + 1, 0x00);  /* divisor hi byte */
	outb(COM1 + 3, 0x03);  /* 8 bits, no parity, one stop bit; clears DLAB */
	outb(COM1 + 2, 0xC7);  /* enable FIFO, clear, 14-byte threshold */
	outb(COM1 + 4, 0x0B);  /* IRQs disabled, RTS/DSR set */
}

static int transmit_empty(void) {
	return inb(COM1 + 5) & 0x20;
}

void serial_putc(char c) {
	if (c == '\n')
		serial_putc('\r');
	while (!transmit_empty())
		;
	outb(COM1, (unsigned char)c);
}

void serial_puts(const char *s) {
	while (*s)
		serial_putc(*s++);
}
