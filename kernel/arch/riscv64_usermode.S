/* GENERATED from a real-mnemonic source by arch/gen_riscv64_asm.sh --
 * do not hand-edit.
 *
 * void enter_usermode(unsigned long entry, unsigned long user_sp)
 *
 * riscv64 equivalent of arch/usermode.S: builds the S-mode -> U-mode
 * transition by hand (sepc = target pc, sstatus.SPP cleared to select
 * U-mode as sret's destination privilege level, sp loaded directly
 * since it's an ordinary register sret doesn't touch), then sret.
 * Never returns, in the same sense as i386's version: execution
 * continues in U-mode at `entry`. No symbol references needed at all
 * -- like sched/riscv64_switch_context.S and unlike boot/riscv64_boot.S
 * or arch/riscv64_trap_entry.S, everything here is register/CSR-based,
 * so no hardcoded-address tricks were needed, just straightforward
 * instruction-by-instruction encoding.
 *
 * sstatus.SPIE (whether interrupts end up enabled once in U-mode) is
 * NOT hardcoded to 1 -- real bug, found running real *paced*
 * interactive input (a live human typing, or Puppeteer simulating
 * one; every scripted/automated test delivers its input all at once,
 * never triggering this): sret copies SPIE into SIE, so hardcoding
 * SPIE=1 here unconditionally re-enabled global interrupts on every
 * call, undoing arch/riscv64_timer.c's timer_disable() the moment
 * checkpoint 5's ring3 test (this function's only caller) ran --
 * timer_disable() exists specifically to stop the timer interrupt
 * from ever nesting inside a syscall's own trap handling again (see
 * its own comment for the full mechanism); this silently brought it
 * right back. Instead, SIE's *current* value (bit 1 of the sstatus
 * this function itself just read into t0, before any modification)
 * is extracted and shifted up 4 bits into SPIE's position (bit 5) --
 * a fixed 4-bit gap between the two fields, so no branch is needed,
 * just arithmetic -- so a call to this function preserves whatever
 * the live global interrupt policy already is, rather than silently
 * overriding it. sched/riscv64_process.c's process_create_from_elf()
 * had the exact same bug, fixed the same way (see its own comment). */
.globl enter_usermode
enter_usermode:
	.long 0x14151073	/* csrw sepc,a0 */
	.long 0x100022f3	/* csrr t0,sstatus */
	.long 0x00200313	/* li t1,2 */
	.long 0x0062f3b3	/* and t2,t0,t1 */
	.long 0x00439393	/* slli t2,t2,0x4 */
	.long 0x12000313	/* li t1,288 */
	.long 0xfff34313	/* not t1,t1 */
	.long 0x0062f2b3	/* and t0,t0,t1 */
	.long 0x0072e2b3	/* or t0,t0,t2 */
	.long 0x10029073	/* csrw sstatus,t0 */
	.long 0x00058113	/* mv sp,a1 */
	.long 0x10200073	/* sret */
