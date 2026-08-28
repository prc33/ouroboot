/* Minimal freestanding ring3 payload: write a message, then exit. */
#include "syscall_riscv64.h"

void _start(void) {
	static const char msg[] = "hello from ring3 via ecall\n";
	riscv_syscall(1, (long)msg, sizeof(msg) - 1, 0, 0, 0, 0, 64); /* SYS_write */
	riscv_syscall(0, 0, 0, 0, 0, 0, 0, 93);                       /* SYS_exit */
	for (;;) {}
}
