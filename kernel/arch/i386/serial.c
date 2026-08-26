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

/* checkpoint 18: real blocking stdin read (syscall_posix.c's own
 * sys_read, shared with riscv64 -- see that function's own comment)
 * needs a way to poll for an available byte and then take it, the
 * i386 counterpart to arch/risc/riscv64_serial.c's own pair. Line
 * status register bit 0 ("Data Ready") is the standard 16550 way to
 * ask "is there a byte waiting" without blocking; interrupts stay
 * disabled the same way serial_init() always left them (this kernel's
 * own SYS_read polls cooperatively, the same "spin-yield" technique
 * process_schedule()'s own callers already use elsewhere, rather than
 * waiting on a real RX interrupt). */
int serial_rx_ready(void) {
	return inb(COM1 + 5) & 0x01;
}

unsigned char serial_getc(void) {
	return inb(COM1);
}
