#ifndef IDT_H
#define IDT_H

/* Matches exactly what arch/i386/isr_stubs.S pushes, low address to high.
 * Four segment registers are saved individually (ds, es, fs, gs), not
 * collapsed into one shared value -- musl's TLS setup (set_thread_area)
 * deliberately makes %gs point at a different selector than %ds/%es/%fs,
 * so restoring all four from a single saved register would silently
 * clobber the TLS selector on every syscall return. Caught before it
 * became a real bug: found while reading how set_thread_area actually
 * needs to work, not from a failing test -- worth fixing before the
 * real musl checkpoint exercises it, rather than after.
 *
 * pusha pushes EAX,ECX,EDX,EBX,ESP(orig),EBP,ESI,EDI in that order, so
 * EDI (pushed last) sits at the lowest address; the four segment saves
 * happen after pusha and are pushed gs,fs,es,ds (see isr_stubs.S), so
 * ds ends up lowest of the four, sitting right below edi. Then
 * int_no/err_code (pushed by the isrN/irqN stub), then whatever the
 * CPU itself pushed on the original interrupt/trap (eip/cs/eflags,
 * plus an implicit useresp/ss below those two on a privilege-level
 * change, which iret consumes automatically and this struct doesn't
 * need to name). */
struct regs {
	unsigned int ds, es, fs, gs;
	unsigned int edi, esi, ebp, esp, ebx, edx, ecx, eax;
	unsigned int int_no, err_code;
	unsigned int eip, cs, eflags;
};

void idt_init(void);
void isr_register_handler(int n, void (*handler)(struct regs *));
void irq_register_handler(int n, void (*handler)(struct regs *));
void syscall_set_handler(void (*handler)(struct regs *));

#endif
