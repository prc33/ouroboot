/* checkpoint 6 test payload: real static musl+TCC riscv64 binary
 * (same pipeline as user_test/hello_riscv64.c), loaded TWICE into two
 * independent processes (sched/riscv64_process.c) with different
 * argv[0] ("A" and "B"). Each writes its label + a loop counter,
 * yields via sched_yield(), and repeats. If cooperative round-robin
 * scheduling across two genuinely separate address spaces really
 * works, the output interleaves: "A1 B1 A2 B2 A3 B3 " -- if it
 * doesn't (e.g. each process actually ran to completion before the
 * other started), it comes out "A1 A2 A3 B1 B2 B3 " instead. A clean,
 * checkable signature, same idea as riscv64_kmain.c's existing P4
 * task-scheduler test ("TASK A: loop 1 (switch 1/6)" etc). */
#include <unistd.h>
#include <sched.h>

int main(int argc, char **argv) {
	char label = (argc > 0 && argv[0][0]) ? argv[0][0] : '?';
	for (int i = 1; i <= 3; i++) {
		char buf[4];
		buf[0] = label;
		buf[1] = '0' + i;
		buf[2] = ' ';
		write(1, buf, 3);
		sched_yield();
	}
	return 0;
}
