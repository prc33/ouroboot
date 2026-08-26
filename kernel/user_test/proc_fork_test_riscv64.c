/* checkpoint 7 test payload: real fork()+wait4(), same real musl+TCC
 * pipeline as every other user_test payload. Deliberately avoids
 * printf/malloc (this checkpoint doesn't touch brk/mmap, which are
 * still file-static globals shared across every process -- see
 * arch/risc/riscv64_syscall.c's file comment -- so exercising them here
 * would be testing a pre-existing, documented simplification this
 * checkpoint doesn't fix, not fork() itself); write() and a tiny
 * hand-rolled decimal formatter are enough to prove what matters:
 *
 *   - `counter` is a real global, written to 1 *before* fork(). The
 *     child then overwrites its own copy to 42 and exits with status
 *     7; if fork() really gave it a copy-on-write *clone* of the
 *     parent's address space (arch/risc/riscv64_paging.c's paging_fork_cow)
 *     rather than e.g. accidentally sharing it, the parent's own
 *     `counter` is untouched by that write -- still 1 when the parent
 *     reads it back after wait4() returns.
 *   - wait4()'s reaped exit status must be exactly the child's real
 *     exit code (7), proving process_exit_current()'s exit_code and
 *     process_wait4()'s reaping are wired together correctly, not
 *     just "wait4() returns *something*". */
#include <unistd.h>
#include <sys/wait.h>

static int counter = 0;

static void write_str(const char *s) {
	int n = 0;
	while (s[n]) n++;
	write(1, s, n);
}

static void write_int(int v) {
	char buf[12];
	int n = 0;
	unsigned int uv;
	if (v == 0) { write(1, "0", 1); return; }
	if (v < 0) { write(1, "-", 1); uv = (unsigned int)(-v); }
	else       { uv = (unsigned int)v; }
	while (uv) { buf[n++] = '0' + (uv % 10); uv /= 10; }
	while (n) write(1, &buf[--n], 1);
}

int main(void) {
	counter = 1;

	pid_t pid = fork();
	if (pid < 0) {
		write_str("fork failed\n");
		return 1;
	}
	if (pid == 0) {
		counter = 42;
		write_str("child counter=");
		write_int(counter);
		write_str("\n");
		_exit(7);
	}

	int status = 0;
	pid_t reaped = wait4(pid, &status, 0, 0);
	write_str("parent counter=");
	write_int(counter);
	write_str(" reaped_pid_ok=");
	write_int(reaped == pid);
	write_str(" child_status=");
	write_int(WEXITSTATUS(status));
	write_str("\n");
	return 0;
}
