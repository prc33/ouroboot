/* Minimal freestanding printf: %d %u %x %X %p %s %c %% plus an 'l'
 * length modifier (%ld %lu %lx %lX) for riscv64's 64-bit CSR/address
 * values -- i386 never needs it (its own longs are 32 bits, same as
 * int), but riscv64_trap.c/riscv64_paging.c genuinely do. No libc. */
#include "kernel.h"
#include <stdarg.h>

static void print_ulong(unsigned long v, unsigned int base, int upper) {
	char buf[32];
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int i = 0;
	if (v == 0) {
		serial_putc('0');
		return;
	}
	while (v) {
		buf[i++] = digits[v % base];
		v /= base;
	}
	while (i > 0)
		serial_putc(buf[--i]);
}

static void print_uint(unsigned int v, unsigned int base, int upper) {
	print_ulong(v, base, upper);
}

static void print_int(int v) {
	if (v < 0) {
		serial_putc('-');
		print_uint((unsigned int)(-(v + 1)) + 1, 10, 0);
	} else {
		print_uint((unsigned int)v, 10, 0);
	}
}

static void print_long(long v) {
	if (v < 0) {
		serial_putc('-');
		print_ulong((unsigned long)(-(v + 1)) + 1, 10, 0);
	} else {
		print_ulong((unsigned long)v, 10, 0);
	}
}

void kprintf(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	for (const char *p = fmt; *p; p++) {
		if (*p != '%') {
			serial_putc(*p);
			continue;
		}
		p++;
		if (*p == 'l') {
			p++;
			switch (*p) {
			case 'd': print_long(va_arg(ap, long)); break;
			case 'u': print_ulong(va_arg(ap, unsigned long), 10, 0); break;
			case 'x': print_ulong(va_arg(ap, unsigned long), 16, 0); break;
			case 'X': print_ulong(va_arg(ap, unsigned long), 16, 1); break;
			case '\0': va_end(ap); return;
			default: serial_putc('%'); serial_putc('l'); serial_putc(*p); break;
			}
			continue;
		}
		switch (*p) {
		case 'd': print_int(va_arg(ap, int)); break;
		case 'u': print_uint(va_arg(ap, unsigned int), 10, 0); break;
		case 'x': print_uint(va_arg(ap, unsigned int), 16, 0); break;
		case 'X': print_uint(va_arg(ap, unsigned int), 16, 1); break;
		case 'p':
			serial_puts("0x");
			print_ulong((unsigned long)va_arg(ap, void *), 16, 0);
			break;
		case 's': serial_puts(va_arg(ap, const char *)); break;
		case 'c': serial_putc((char)va_arg(ap, int)); break;
		case '%': serial_putc('%'); break;
		case '\0': va_end(ap); return;
		default: serial_putc('%'); serial_putc(*p); break;
		}
	}
	va_end(ap);
}
