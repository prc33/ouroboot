/* Real static musl+TCC binary for the riscv64 ELF-loader checkpoint --
 * same role and same output as i386's user_test/hello.elf (a copy of
 * the original compiler-spike binary, source not checked in there).
 * See docs/riscv-port-findings.md for the riscv64 musl+TCC pipeline
 * this is built with (compiler/tcc TARGET=riscv64 + demo/musl-riscv64,
 * both already verified independently of the kernel by
 * demo/build-musl-riscv64.sh's own smoke test). */
#include <stdio.h>
#include <stdlib.h>
int main(void) {
	int *buf = malloc(10 * sizeof(int));
	int sum = 0;
	for (int i = 0; i < 10; i++) { buf[i] = i * i; sum += buf[i]; }
	free(buf);
	printf("hello from musl+tcc, sum=%d\n", sum);
	return 0;
}
