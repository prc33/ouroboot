/* GENERATED from a real-mnemonic source by arch/gen_riscv64_asm.sh --
 * do not hand-edit.
 *
 * void enter_usermode(unsigned long entry, unsigned long user_sp)
 *
 * riscv64 equivalent of arch/usermode.S: builds the S-mode -> U-mode
 * transition by hand (sepc = target pc, sstatus.SPP cleared to select
 * U-mode as sret's destination privilege level, sstatus.SPIE set so
 * interrupts are enabled once there, sp loaded directly since it's an
 * ordinary register sret doesn't touch), then sret. Never returns, in
 * the same sense as i386's version: execution continues in U-mode at
 * `entry`. No symbol references needed at all -- like
 * sched/riscv64_switch_context.S and unlike boot/riscv64_boot.S or
 * arch/riscv64_trap_entry.S, everything here is register/CSR-based,
 * so no hardcoded-address tricks were needed, just straightforward
 * instruction-by-instruction encoding. */
.globl enter_usermode
enter_usermode:
	.long 0x14151073	/* csrw sepc,a0 */
	.long 0x100022f3	/* csrr t0,sstatus */
	.long 0x10000313	/* li t1,256 */
	.long 0xfff34313	/* not t1,t1 */
	.long 0x0062f2b3	/* and t0,t0,t1 */
	.long 0x0202e293	/* ori t0,t0,32 */
	.long 0x10029073	/* csrw sstatus,t0 */
	.long 0x00058113	/* mv sp,a1 */
	.long 0x10200073	/* sret */
