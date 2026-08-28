#ifndef RISCV64_TRAP_H
#define RISCV64_TRAP_H

/* Matches exactly what arch/risc/riscv64_trap_entry.S saves, low offset to
 * high (all fields are 8 bytes, no padding -- offsets below are the
 * field index * 8, and arch/risc/riscv64_trap_entry.S uses these same
 * offsets by hand). x0 (always zero) is never saved; every
 * other GPR (x1-x31) is, plus the four trap CSRs. */
struct regs {
	unsigned long ra, sp, gp, tp;               /*  0, 8, 16, 24 */
	unsigned long t0, t1, t2;                   /* 32, 40, 48 */
	unsigned long s0, s1;                       /* 56, 64 */
	unsigned long a0, a1, a2, a3, a4, a5, a6, a7; /* 72..128 */
	unsigned long s2, s3, s4, s5, s6, s7, s8, s9, s10, s11; /* 136..208 */
	unsigned long t3, t4, t5, t6;               /* 216, 224, 232, 240 */
	unsigned long sepc, sstatus, scause, stval; /* 248, 256, 264, 272 */
};

void trap_init(void);
void isr_register_handler(int cause, void (*handler)(struct regs *)); /* exceptions (scause high bit clear) */
void irq_register_handler(int cause, void (*handler)(struct regs *)); /* interrupts (scause high bit set) */
void syscall_set_handler(void (*handler)(struct regs *));

/* Called by arch/risc/riscv64_timer.c's tick handler -- see arch/risc/riscv64_trap.c. */
void trap_dispatch(struct regs *r);

#endif
