/* GENERATED from a real-mnemonic source by arch/gen_riscv64_asm.sh --
 * do not hand-edit (except the .rept-collapsed padding, added by hand
 * for readability; the 7 real instructions are exactly the
 * generator's raw output for `li sp, 0x80604000; li t0, 0x80201000;
 * jalr x0, 0(t0)` -- `li` expands to a 3-instruction lui+addiw+slli
 * sequence here rather than the more obvious lui+addi, because a
 * naive `lui rd, 0x80304` sign-extends: 0x80304's own bit 19 is set,
 * so plain lui alone produces 0xffffffff80304000, not 0x80304000 --
 * hand-encoding this wrong was this port's first real bug, caught by
 * booting and finding the CPU jumped into unmapped high memory
 * instead of RAM; `li` is the pseudo-op that gets this right).
 *
 * Entry point. Nothing before this has run -- sp is garbage. Raw asm's
 * ONLY job: set sp to a fixed address we control (RV64_BOOT_STACK_TOP,
 * see arch/risc/riscv64_memmap.h), then jump to the first byte of the next
 * linked object file's compiled code.
 *
 * Why a jump to a hardcoded absolute address instead of "call kmain":
 * riscv64 TCC's assembler has no relocation support at all for hand-
 * written .S files (confirmed: .quad/.long directives accept only
 * numeric constants, not symbols -- see tccasm.c's TOK_ASMDIR_quad/
 * long -- unlike real per-C-function codegen, which handles this correctly
 * but only for .c source). So this file is padded with nops to exactly
 * one page (0x1000 bytes); as long as it's linked first (kernel/
 * Makefile enforces this) and TCC's linker concatenates object files'
 * sections in link order with no gaps (already relied on by kend.S),
 * the very next linked object's .text starts at exactly
 * KERNEL_LINK_BASE + 0x1000 = 0x80201000 -- a hardcoded constant.
 * riscv64_entry.c's _riscv64_entry (a normal, ordinarily-compiled C
 * function -- no tricks needed once we're this far) must be the
 * first and only function TCC emits for that object file, and that
 * object must be the second one linked, right after this one.
 */
.globl _start
_start:
	.long 0x00020137	/* lui sp,0x20 */
	.long 0x1811011b	/* addiw sp,sp,385 -- sp now 0x20181 */
	.long 0x00e11113	/* slli sp,sp,0xe -- sp = 0x80604000 (RV64_BOOT_STACK_TOP) */
	.long 0x000802b7	/* lui t0,0x80 */
	.long 0x2012829b	/* addiw t0,t0,513 -- t0 now 0x80201 */
	.long 0x00c29293	/* slli t0,t0,0xc -- t0 = 0x80201000 (KERNEL_LINK_BASE + 0x1000) */
	.long 0x00028067	/* jr t0 */
	.rept (4096 - 28) / 4
	.long 0x00000013	/* nop -- pads this file to exactly one page */
	.endr
