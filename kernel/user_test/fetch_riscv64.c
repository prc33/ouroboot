#include <stdio.h>

int main(int argc, char **argv) {
	if (argc != 3) {
		fputs("usage: fetch URL FILE\n", stderr);
		return 2;
	}
	long n = __builtin_riscv_syscall((long)argv[1], (long)argv[2], 0, 0, 0, 0, 0, 1000);
	if (n < 0) {
		fprintf(stderr, "fetch failed: %ld\n", n);
		return 1;
	}
	printf("fetched %ld bytes to %s\n", n, argv[2]);
	return 0;
}
