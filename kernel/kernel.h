#ifndef KERNEL_H
#define KERNEL_H

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);

void kprintf(const char *fmt, ...);

#ifndef KERNEL_ARCH_RISCV64
void kmain(unsigned int magic, unsigned int mb_info);

void gdt_init(void);
void tss_set_kernel_stack(unsigned int esp0);
void gdt_set_tls_entry(int index, unsigned int base);
void idt_init(void);
void syscall_init(void);
void enter_usermode(unsigned int entry, unsigned int user_esp);
void run_elf_test(void); /* kmain.c -- see arch/i386/syscall.c's sys_exit */
void pic_remap(void);
void pic_send_eoi(unsigned char irq);
void pic_set_mask(unsigned char irq);
void pic_clear_mask(unsigned char irq);
void pit_init(unsigned int hz);
#else
/* -- riscv64: see arch/risc/riscv64_trap.h/.c, arch/risc/riscv64_timer.c,
 * arch/risc/riscv64_syscall.c, arch/risc/riscv64_usermode.S. No GDT/PIC/PIT
 * equivalents exist -- no segmentation, and the timer is CSR-based
 * (Sstc), not a discrete PIC-driven chip. */
void kmain(unsigned long hartid, unsigned long dtb);
void trap_init(void);
void timer_init(unsigned int hz);
void timer_set_tick_handler(void (*handler)(void)); /* arch/risc/riscv64_timer.c */
void timer_disable(void); /* arch/risc/riscv64_timer.c -- see its own comment: only safe past the P4 scheduler checkpoint */
void syscall_init(void);
void enter_usermode(unsigned long entry, unsigned long user_sp);
int serial_rx_ready(void);   /* arch/risc/riscv64_serial.c -- checkpoint 8, blocking stdin reads */
unsigned char serial_getc(void);
#endif

#endif
