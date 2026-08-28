#ifndef SYSCALL_RISCV64_H
#define SYSCALL_RISCV64_H

static inline long riscv_syscall(long a, long b, long c, long d,
	long e, long f, long g, long n)
{
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	register long a2 __asm__("a2") = c;
	register long a3 __asm__("a3") = d;
	register long a4 __asm__("a4") = e;
	register long a5 __asm__("a5") = f;
	register long a6 __asm__("a6") = g;
	register long a7 __asm__("a7") = n;
	__asm__ volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3),
		"r"(a4), "r"(a5), "r"(a6), "r"(a7) : "memory");
	return a0;
}

#endif
