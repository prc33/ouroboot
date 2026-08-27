/* Real static musl+TCC i386 binary for the P5 checkpoint 2 ELF-loader
 * test -- checkpoint 20 (docs/repo-review-2026-08-26.md section 4):
 * this checkpoint used to embed a pre-built copy of this exact program
 * as a 44,228-byte hex dump (arch/i386/hello_elf_payload.h, 3,696
 * lines -- 28% of the whole kernel's source and 47% of the built
 * i386 kernel.elf image), left over from the original compiler-spike
 * binary whose own source was never checked in. Loaded from the
 * checkpoint initrd instead now, the same way test/riscv64_checkpoints.c's
 * own P5 checkpoint 2 already loads user_test/hello_riscv64.c's built
 * ELF -- same role, same output, same musl+TCC pipeline, just i386's
 * own compiler/tcc TARGET=i386 + demo/musl-i386 instead. */
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
