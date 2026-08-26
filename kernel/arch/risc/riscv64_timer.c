/* Sstc extension timer, S-mode direct (no SBI ecall needed to arm it
 * -- confirmed available on QEMU's riscv64 virt machine, see
 * docs/riscv-port-findings.md). This is the preemption clock for the
 * scheduler test, riscv64 equivalent of arch/pit.c.
 *
 * Genuinely different from the 8254 PIT in one respect, not just a
 * different register interface: the PIT free-runs and re-fires on its
 * own once programmed (mode 3, square wave); RISC-V's timer is
 * one-shot -- `stimecmp` fires exactly once when `time` reaches it,
 * then stays quiet until software reprograms it. So every tick has to
 * rearm itself, which is why this file owns the interrupt handler
 * (cause 5, supervisor timer interrupt) itself, rather than leaving
 * that entirely to the caller the way arch/pit.c does -- rearming is
 * not optional per tick here, so it can't be left to a caller that
 * might forget it. timer_set_tick_handler() lets kmain.c hook in its
 * own per-tick work (incrementing g_ticks) without needing to know
 * about rearming at all, keeping its usage pattern close to i386's
 * irq_register_handler(0, timer_handler). */
#include "kernel.h"
#include "riscv64_trap.h"

#define CSR_SIE       0x104
#define CSR_SSTATUS   0x100
#define CSR_TIME      0xC01
#define CSR_STIMECMP  0x14D

#define SIE_STIE     (1UL << 5)
#define SSTATUS_SIE  (1UL << 1)

/* QEMU riscv64 virt machine's fixed timebase frequency -- confirmed
 * in OpenSBI's own boot banner ("Platform Timer Device : aclint-mtimer
 * @ 10000000Hz"), hardcoded rather than parsed from the devicetree,
 * same "we fully control the QEMU invocation" reasoning as
 * arch/risc/riscv64_memmap.h. */
#define TIMEBASE_HZ 10000000UL

static unsigned long g_delta;
static void (*g_tick_handler)(void);

static void timer_irq(struct regs *r) {
	(void)r;
	unsigned long now = __builtin_riscv_csrr(CSR_TIME);
	__builtin_riscv_csrw(CSR_STIMECMP, now + g_delta);
	if (g_tick_handler)
		g_tick_handler();
}

void timer_set_tick_handler(void (*handler)(void)) {
	g_tick_handler = handler;
}

void timer_init(unsigned int hz) {
	g_delta = TIMEBASE_HZ / hz;

	irq_register_handler(5, timer_irq); /* scause 5 = supervisor timer interrupt */

	unsigned long now = __builtin_riscv_csrr(CSR_TIME);
	__builtin_riscv_csrw(CSR_STIMECMP, now + g_delta);

	unsigned long sie = __builtin_riscv_csrr(CSR_SIE);
	__builtin_riscv_csrw(CSR_SIE, sie | SIE_STIE);
	unsigned long sstatus = __builtin_riscv_csrr(CSR_SSTATUS);
	__builtin_riscv_csrw(CSR_SSTATUS, sstatus | SSTATUS_SIE);

	kprintf("timer: Sstc armed at %u Hz (delta=%lu timebase ticks)\n", hz, g_delta);
}

/* Real bug, found running real *paced* interactive input (a live
 * human typing in the browser demo, or Puppeteer simulating one --
 * every existing automated test feeds its scripted input all at once,
 * before the CPU even starts or over an instantly-drained pipe, so
 * arch/risc/riscv64_syscall.c's sys_read (fd==0) never actually spins in
 * its "while (!serial_rx_ready()) process_schedule();" wait for more
 * than a handful of instructions; a real keystroke's real gap is
 * plenty of *virtual* time -- at this 100Hz and the emulator's own
 * --time-advance 100 -- for many timer interrupts to come due while
 * still deep inside that loop). A timer interrupt is a trap like any
 * other, routed through the exact same single global trapframe
 * (arch/risc/riscv64_memmap.h's RV64_TRAPFRAME_BASE) an outer ecall is
 * still relying on -- this kernel's whole trap design assumes traps
 * never nest (arch/risc/riscv64_trap_entry.S's own comment), and an
 * interrupt firing mid-syscall is exactly that nested trap. Unlike
 * arch/risc/riscv64_paging.c's paging_ensure_writable() (the same class of
 * bug via a page fault instead), there's no way to "pre-resolve" an
 * asynchronous interrupt before it can fire -- the only fix that
 * doesn't mean rewriting this kernel's trap handling for real nested
 * contexts is to stop it happening at all.
 *
 * Nothing past the P4 scheduler checkpoint (kmain.c's own
 * conclude_scheduler_test(), the only caller) actually needs the
 * timer any more -- every process/syscall switch from P5 onward is
 * already cooperative (sched_yield/wait4/read all reach
 * process_schedule() by calling it directly, never via a timer ISR),
 * so turning interrupts off for good here trades away nothing real. */
void timer_disable(void) {
	unsigned long sstatus = __builtin_riscv_csrr(CSR_SSTATUS);
	__builtin_riscv_csrw(CSR_SSTATUS, sstatus & ~SSTATUS_SIE);
}
