/* The trampoline arch/riscv64_boot.S's hardcoded jump lands on. Must
 * be the only function in this file, and this object must be the
 * second one linked (right after riscv64_boot.o) -- see that file's
 * comment for why. Everything from here on is ordinary compiled C;
 * no more raw-asm tricks are needed, since TCC's real riscv64 codegen
 * (unlike its .S-file assembler) handles calling other C functions
 * correctly -- proven throughout the compiler port's own musl/busybox
 * test suites.
 */
void kmain(unsigned long hartid, unsigned long dtb);

void _riscv64_entry(void) {
	kmain(0, 0); /* hartid/dtb unused -- see arch/riscv64_memmap.h */
	for (;;)
		__builtin_riscv_wfi();
}
