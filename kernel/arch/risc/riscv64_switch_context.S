/* GENERATED from a real-mnemonic source by arch/gen_riscv64_asm.sh --
 * do not hand-edit.
 *
 * void switch_context(unsigned long *old_sp_store, unsigned long new_sp)
 *
 * Same shape as arch/i386/switch_context.S's i386 version: saves the
 * callee-saved registers and current sp into *old_sp_store, switches
 * sp to new_sp, restores the other task's registers from there, and
 * `ret`s -- which lands wherever that stack's saved return address
 * points (see arch/risc/riscv64_task.c's task_init for the hand-built
 * initial stack). riscv64's calling convention has 13 callee-saved
 * registers (ra + s0-s11) vs i386's 4 (ebp/edi/esi/ebx) -- more to
 * save, otherwise identical technique. No symbol references needed
 * at all here (unlike arch/risc/riscv64_boot.S or
 * arch/risc/riscv64_trap_entry.S) -- everything is register-argument-based,
 * so unlike those two this file needed no special hardcoded-address
 * handling, just straightforward instruction-by-instruction encoding.
 */
.globl switch_context
switch_context:
	.long 0xf9010113	/* addi sp,sp,-112 */
	.long 0x00113023	/* sd ra,0(sp) */
	.long 0x00813423	/* sd s0,8(sp) */
	.long 0x00913823	/* sd s1,16(sp) */
	.long 0x01213c23	/* sd s2,24(sp) */
	.long 0x03313023	/* sd s3,32(sp) */
	.long 0x03413423	/* sd s4,40(sp) */
	.long 0x03513823	/* sd s5,48(sp) */
	.long 0x03613c23	/* sd s6,56(sp) */
	.long 0x05713023	/* sd s7,64(sp) */
	.long 0x05813423	/* sd s8,72(sp) */
	.long 0x05913823	/* sd s9,80(sp) */
	.long 0x05a13c23	/* sd s10,88(sp) */
	.long 0x07b13023	/* sd s11,96(sp) */
	.long 0x00253023	/* sd sp,0(a0) */
	.long 0x00058113	/* mv sp,a1 */
	.long 0x00013083	/* ld ra,0(sp) */
	.long 0x00813403	/* ld s0,8(sp) */
	.long 0x01013483	/* ld s1,16(sp) */
	.long 0x01813903	/* ld s2,24(sp) */
	.long 0x02013983	/* ld s3,32(sp) */
	.long 0x02813a03	/* ld s4,40(sp) */
	.long 0x03013a83	/* ld s5,48(sp) */
	.long 0x03813b03	/* ld s6,56(sp) */
	.long 0x04013b83	/* ld s7,64(sp) */
	.long 0x04813c03	/* ld s8,72(sp) */
	.long 0x05013c83	/* ld s9,80(sp) */
	.long 0x05813d03	/* ld s10,88(sp) */
	.long 0x06013d83	/* ld s11,96(sp) */
	.long 0x07010113	/* addi sp,sp,112 */
	.long 0x00008067	/* ret */
