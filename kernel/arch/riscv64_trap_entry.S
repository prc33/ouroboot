/* GENERATED from a real-mnemonic source by arch/gen_riscv64_asm.sh --
 * do not hand-edit.
 *
 * The one stvec target for every trap on this kernel: exceptions,
 * interrupts, and `ecall` (our syscall gate) alike -- RISC-V has a
 * single unified vector, unlike i386's 256-entry IDT (genuinely
 * simpler here). All addresses below (0x80306000 etc) are hardcoded
 * constants from arch/riscv64_memmap.h, not symbols -- this file is
 * raw .long-encoded machine code, and riscv64 TCC's assembler has no
 * relocation support for hand-written .S files at all (see
 * boot/riscv64_boot.S's comment for the full story).
 *
 * Technique: `csrrw t6, sscratch, t6` bootstraps ONE scratch register
 * (t6's real value is parked in the sscratch CSR, off to the side of
 * the 31 GPRs, since every GPR is still live with the interrupted
 * program's values and none can be clobbered without a backup first;
 * sscratch's *pre-trap* value is never read, only used as temporary
 * parking, so it doesn't need initializing). t6 now free to use:
 * loaded with RV64_TRAPFRAME_BASE, then every other GPR is stored at
 * a fixed offset into the global struct regs (arch/riscv64_trap.h --
 * offsets here must match that struct's field order exactly). t6's
 * own real value is retrieved from sscratch and stored last. Then
 * sepc/sstatus/scause/stval, then switch to *this trap's* kernel
 * stack, load trap_dispatch's address from RV64_TRAP_DISPATCH_PTR
 * (written there at runtime by arch/riscv64_trap.c's trap_init(),
 * *not* baked in here -- this is how raw asm calls into compiled C
 * without a relocation it can't have) and call it with a0 = &trapframe.
 * On return (via falling out of trap_dispatch normally, or via the
 * riscv64_trap_return label below being jumped to directly -- see
 * that label's own comment): restore sepc/sstatus into their CSRs,
 * restore every GPR (t6 last, via a valid self-referential
 * `ld t6, 240(t6)` -- RISC-V loads compute the address from the OLD
 * register value before overwriting it), sret.
 *
 * Stack switch, checkpoint 6 change: earlier checkpoints (P1-P5)
 * loaded sp from a single hardcoded immediate, RV64_TRAP_STACK_TOP --
 * safe when there's only ever one flow of control that could trap.
 * sched/riscv64_process.c's general process table breaks that: a
 * process can genuinely suspend *mid-syscall* (sched_yield, and later
 * wait4/read) and a *different* process can then trap while the first
 * is still parked partway through its own trap_dispatch() call chain.
 * If both used the same physical trap stack, the second process's
 * trap would overwrite memory the first's suspended C call chain is
 * still sitting on. Fix: sp is now loaded *indirectly*, from
 * RV64_CURRENT_KSTACK_PTR (same "runtime-populated pointer slot"
 * trick as RV64_TRAP_DISPATCH_PTR, for the same underlying reason --
 * no relocation support to bake in a per-process address here even if
 * there were a single correct one). sched/riscv64_process.c's
 * scheduler keeps that slot pointed at whichever process is about to
 * run in U-mode, so trap handling for *that* process always lands on
 * *that* process's own kernel stack -- unifying "trap stack" and
 * "process kernel stack" into one, the same design real Unix kernels
 * use, rather than keeping them as two separate concepts. Every
 * process's kernel stack must therefore be initialized (once, before
 * that process ever traps) exactly the way this file expects to find
 * one: nothing in particular, since sp is simply overwritten here --
 * the only real requirement is that RV64_CURRENT_KSTACK_PTR names a
 * valid, sufficiently-large, currently-unused stack region.
 *
 * riscv64_trap_return: a second, later entry point into this same
 * body -- everything from here to `sret` is byte-identical to what
 * P1-P5 already had and already verified (restore-from-trapframe is
 * inherently stateless: it only ever reads RV64_TRAPFRAME_BASE fresh,
 * never anything left over from the save half above), just now also
 * reachable directly. sched/riscv64_process.c's process-resume
 * trampoline jumps here (not through a real `ecall`/trap) the first
 * time a process is dispatched: having already written that process's
 * saved regs into RV64_TRAPFRAME_BASE and pointed satp/
 * RV64_CURRENT_KSTACK_PTR at it, "resume via the same path a real
 * trap-return would use" is both correct and the only path that
 * already existed and was already proven, rather than a second,
 * parallel, unverified restore sequence. */
.globl riscv64_trap_entry
riscv64_trap_entry:
	.long 0x140f9ff3	/* csrrw t6,sscratch,t6 */
	.long 0x00040fb7	/* lui t6,0x40 */
	.long 0x183f8f9b	/* addiw t6,t6,387 # 40183 <riscv64_trap_return+0x4009f> */
	.long 0x00df9f93	/* slli t6,t6,0xd */
	.long 0x001fb023	/* sd ra,0(t6) */
	.long 0x002fb423	/* sd sp,8(t6) */
	.long 0x003fb823	/* sd gp,16(t6) */
	.long 0x004fbc23	/* sd tp,24(t6) */
	.long 0x025fb023	/* sd t0,32(t6) */
	.long 0x026fb423	/* sd t1,40(t6) */
	.long 0x027fb823	/* sd t2,48(t6) */
	.long 0x028fbc23	/* sd s0,56(t6) */
	.long 0x049fb023	/* sd s1,64(t6) */
	.long 0x04afb423	/* sd a0,72(t6) */
	.long 0x04bfb823	/* sd a1,80(t6) */
	.long 0x04cfbc23	/* sd a2,88(t6) */
	.long 0x06dfb023	/* sd a3,96(t6) */
	.long 0x06efb423	/* sd a4,104(t6) */
	.long 0x06ffb823	/* sd a5,112(t6) */
	.long 0x070fbc23	/* sd a6,120(t6) */
	.long 0x091fb023	/* sd a7,128(t6) */
	.long 0x092fb423	/* sd s2,136(t6) */
	.long 0x093fb823	/* sd s3,144(t6) */
	.long 0x094fbc23	/* sd s4,152(t6) */
	.long 0x0b5fb023	/* sd s5,160(t6) */
	.long 0x0b6fb423	/* sd s6,168(t6) */
	.long 0x0b7fb823	/* sd s7,176(t6) */
	.long 0x0b8fbc23	/* sd s8,184(t6) */
	.long 0x0d9fb023	/* sd s9,192(t6) */
	.long 0x0dafb423	/* sd s10,200(t6) */
	.long 0x0dbfb823	/* sd s11,208(t6) */
	.long 0x0dcfbc23	/* sd t3,216(t6) */
	.long 0x0fdfb023	/* sd t4,224(t6) */
	.long 0x0fefb423	/* sd t5,232(t6) */
	.long 0x14002f73	/* csrr t5,sscratch */
	.long 0x0fefb823	/* sd t5,240(t6) */
	.long 0x14102f73	/* csrr t5,sepc */
	.long 0x0fefbc23	/* sd t5,248(t6) */
	.long 0x10002f73	/* csrr t5,sstatus */
	.long 0x11efb023	/* sd t5,256(t6) */
	.long 0x14202f73	/* csrr t5,scause */
	.long 0x11efb423	/* sd t5,264(t6) */
	.long 0x14302f73	/* csrr t5,stval */
	.long 0x11efb823	/* sd t5,272(t6) */
	.long 0x00080f37	/* lui t5,0x80 */
	.long 0x305f0f1b	/* addiw t5,t5,773 # 80305 <riscv64_trap_return+0x80221> */
	.long 0x00cf1f13	/* slli t5,t5,0xc */
	.long 0x008f0f13	/* addi t5,t5,8 */
	.long 0x000f3103	/* ld sp,0(t5) */
	.long 0x00040537	/* lui a0,0x40 */
	.long 0x1835051b	/* addiw a0,a0,387 # 40183 <riscv64_trap_return+0x4009f> */
	.long 0x00d51513	/* slli a0,a0,0xd */
	.long 0x000802b7	/* lui t0,0x80 */
	.long 0x3052829b	/* addiw t0,t0,773 # 80305 <riscv64_trap_return+0x80221> */
	.long 0x00c29293	/* slli t0,t0,0xc */
	.long 0x0002b283	/* ld t0,0(t0) */
	.long 0x000280e7	/* jalr t0 */
.globl riscv64_trap_return
riscv64_trap_return:
	.long 0x00040fb7	/* lui t6,0x40 */
	.long 0x183f8f9b	/* addiw t6,t6,387 # 40183 <riscv64_trap_return+0x4009f> */
	.long 0x00df9f93	/* slli t6,t6,0xd */
	.long 0x0f8fbf03	/* ld t5,248(t6) */
	.long 0x141f1073	/* csrw sepc,t5 */
	.long 0x100fbf03	/* ld t5,256(t6) */
	.long 0x100f1073	/* csrw sstatus,t5 */
	.long 0x000fb083	/* ld ra,0(t6) */
	.long 0x008fb103	/* ld sp,8(t6) */
	.long 0x010fb183	/* ld gp,16(t6) */
	.long 0x018fb203	/* ld tp,24(t6) */
	.long 0x020fb283	/* ld t0,32(t6) */
	.long 0x028fb303	/* ld t1,40(t6) */
	.long 0x030fb383	/* ld t2,48(t6) */
	.long 0x038fb403	/* ld s0,56(t6) */
	.long 0x040fb483	/* ld s1,64(t6) */
	.long 0x048fb503	/* ld a0,72(t6) */
	.long 0x050fb583	/* ld a1,80(t6) */
	.long 0x058fb603	/* ld a2,88(t6) */
	.long 0x060fb683	/* ld a3,96(t6) */
	.long 0x068fb703	/* ld a4,104(t6) */
	.long 0x070fb783	/* ld a5,112(t6) */
	.long 0x078fb803	/* ld a6,120(t6) */
	.long 0x080fb883	/* ld a7,128(t6) */
	.long 0x088fb903	/* ld s2,136(t6) */
	.long 0x090fb983	/* ld s3,144(t6) */
	.long 0x098fba03	/* ld s4,152(t6) */
	.long 0x0a0fba83	/* ld s5,160(t6) */
	.long 0x0a8fbb03	/* ld s6,168(t6) */
	.long 0x0b0fbb83	/* ld s7,176(t6) */
	.long 0x0b8fbc03	/* ld s8,184(t6) */
	.long 0x0c0fbc83	/* ld s9,192(t6) */
	.long 0x0c8fbd03	/* ld s10,200(t6) */
	.long 0x0d0fbd83	/* ld s11,208(t6) */
	.long 0x0d8fbe03	/* ld t3,216(t6) */
	.long 0x0e0fbe83	/* ld t4,224(t6) */
	.long 0x0e8fbf03	/* ld t5,232(t6) */
	.long 0x0f0fbf83	/* ld t6,240(t6) */
	.long 0x10200073	/* sret */
