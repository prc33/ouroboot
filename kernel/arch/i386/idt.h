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
 * CPU itself pushed on the original interrupt/trap: eip/cs/eflags
 * always, plus useresp/ss too when the interrupt crossed a privilege
 * level (ring3->ring0) -- every real syscall/user fault this kernel's
 * process layer cares about. isr_stubs.S never touches those last two
 * words (they're simply never popped before its final `iret`, which
 * is exactly what makes the ordinary one-shot ring3 demo work without
 * this struct needing to know they're there at all) -- checkpoint 17
 * (arch/i386/process.c) is the first code that actually needs to name
 * them: a process being *rescheduled* has no live call stack to fall
 * back into, so its saved useresp/ss have to come from somewhere other
 * than "still sitting untouched a few words up the stack". Naming them
 * here means process_arch_save_trapframe()'s plain word-copy loop
 * (arch/i386/process.c) picks them up for free, same technique
 * arch/risc/riscv64_process.c's copy_regs() already uses. Only ever
 * meaningful for a trap that came from ring3 -- reading them for a
 * kernel-mode-only interrupt (never done anywhere in this kernel)
 * would read whatever two words happen to sit above eflags on that
 * stack, not real user register state. */
struct regs {
	unsigned int ds, es, fs, gs;
	unsigned int edi, esi, ebp, esp, ebx, edx, ecx, eax;
	unsigned int int_no, err_code;
	unsigned int eip, cs, eflags, useresp, ss;
};

void idt_init(void);
void isr_register_handler(int n, void (*handler)(struct regs *));
void irq_register_handler(int n, void (*handler)(struct regs *));
void syscall_set_handler(void (*handler)(struct regs *));

/* Whichever trapframe is currently live -- i.e. whatever isr_handler()/
 * irq_handler() were last called with. arch/i386/process.c's
 * process_arch_save_trapframe() reads this rather than taking a
 * parameter, the i386 equivalent of riscv64's fixed RV64_TRAPFRAME_BASE
 * address (there's no single fixed address here -- the frame lives on
 * whichever kernel stack the trap actually occurred on -- so a pointer
 * that gets reset on every trap entry plays the same role). */
struct regs *idt_current_trapframe(void);

#endif
