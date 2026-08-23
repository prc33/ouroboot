/* Minimal 16550 UART driver, MMIO instead of drivers/serial.c's port
 * I/O -- QEMU's riscv64 `virt` machine puts a 16550-compatible UART
 * at physical 0x10000000 (confirmed empirically -- see
 * docs/riscv-port-findings.md), byte-addressed, same register layout
 * as the i386 version's COM1. See plan decision D8 (serial console
 * only).
 */
#include "kernel.h"

#define UART0 0x10000000UL

static inline void mmio_write8(unsigned long addr, unsigned char val) {
	*(volatile unsigned char *)addr = val;
}

static inline unsigned char mmio_read8(unsigned long addr) {
	return *(volatile unsigned char *)addr;
}

void serial_init(void) {
	mmio_write8(UART0 + 1, 0x00); /* disable interrupts */
	mmio_write8(UART0 + 3, 0x80); /* enable DLAB (set baud rate divisor) */
	mmio_write8(UART0 + 0, 0x03); /* divisor lo byte: 115200 / 3 = 38400 baud */
	mmio_write8(UART0 + 1, 0x00); /* divisor hi byte */
	mmio_write8(UART0 + 3, 0x03); /* 8 bits, no parity, one stop bit; clears DLAB */
	mmio_write8(UART0 + 2, 0xC7); /* enable FIFO, clear, 14-byte threshold */
	mmio_write8(UART0 + 4, 0x0B); /* IRQs disabled (we poll), RTS/DSR set */
}

static int transmit_empty(void) {
	return mmio_read8(UART0 + 5) & 0x20;
}

void serial_putc(char c) {
	if (c == '\n')
		serial_putc('\r');
	while (!transmit_empty())
		;
	mmio_write8(UART0, (unsigned char)c);
}

void serial_puts(const char *s) {
	while (*s)
		serial_putc(*s++);
}
