/* checkpoint 10 test payload: the actual "interactively" half of the
 * P4 exit criterion (docs/emulator-plan.md) -- real busybox ash
 * forced into interactive mode (-i, since this kernel has no tty
 * layer for isatty() to genuinely detect one -- see
 * arch/risc/riscv64_syscall.c's sys_ioctl comment) reading real commands
 * from real stdin (fd 0, the UART -- arch/risc/riscv64_serial.c's
 * serial_rx_ready/serial_getc, arch/risc/riscv64_syscall.c's sys_read),
 * not from a ramfs script file the way checkpoint 9's test.sh was.
 * No fork needed here: this process *becomes* ash directly, same as
 * a real login shell would. */
#include <unistd.h>

int main(void) {
	char *argv[] = { "ash", "-i", 0 };
	char *envp[] = { 0 };
	execve("ash", argv, envp);
	return 97;
}
