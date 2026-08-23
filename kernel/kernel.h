#ifndef KERNEL_H
#define KERNEL_H

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);

void kprintf(const char *fmt, ...);

void kmain(unsigned int magic, unsigned int mb_info);

void gdt_init(void);
void tss_set_kernel_stack(unsigned int esp0);
void gdt_set_tls_entry(int index, unsigned int base);
void idt_init(void);
void syscall_init(void);
void enter_usermode(unsigned int entry, unsigned int user_esp);
void run_elf_test(void); /* kmain.c -- see arch/syscall.c's sys_exit */
void pic_remap(void);
void pic_send_eoi(unsigned char irq);
void pic_set_mask(unsigned char irq);
void pic_clear_mask(unsigned char irq);
void pit_init(unsigned int hz);

#endif
