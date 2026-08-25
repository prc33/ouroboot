/* Minimal ring3 payload, no libc: write(1, msg, len); exit(0). Same
 * role as user_test/user_test.S's i386 version, but written in C
 * instead of hand assembly -- riscv64 TCC has no assembler at all
 * (confirmed throughout this port), while it *does* have
 * __builtin_riscv_syscall (added during the compiler port
 * specifically so kernel/libc code like this doesn't need one: it
 * reuses the ordinary C call path, since the RV64 C ABI already
 * places the first 8 args in a0-a7, which is exactly the Linux
 * syscall convention -- args first, syscall number last).
 *
 * Compiled as a freestanding ELF at 0x800000 and loaded from the initrd.
 * It remains independent of libc; the ELF container merely avoids turning
 * the resulting machine code into a generated C header.
 */
void _start(void) {
	static const char msg[] = "hello from ring3 via ecall\n";
	/* __builtin_riscv_syscall always takes exactly 8 args (real args
	 * first, zero-padded, syscall number last) -- the number only
	 * lands in a7 (the real Linux syscall-number register) because
	 * it's the *8th* positional C argument; fewer args just leaves it
	 * in whatever a-register a shorter call naturally uses instead
	 * (confirmed the hard way: a 4-arg call put 64 in a3, not a7).
	 * Same pattern musl's own __syscallN wrappers use
	 * (arch/riscv64/syscall_arch.h). */
	__builtin_riscv_syscall(1, (long)msg, sizeof(msg) - 1, 0, 0, 0, 0, 64); /* SYS_write */
	__builtin_riscv_syscall(0, 0, 0, 0, 0, 0, 0, 93);                       /* SYS_exit */
	for (;;) {}
}
