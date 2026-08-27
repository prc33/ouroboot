/* checkpoint 19 test payload: real fork()+wait4() on i386, the exact
 * same source as user_test/proc_fork_test_riscv64.c (see that file's
 * own comment for the full rationale) -- reused unchanged rather than
 * rewritten, matching the "genericize/reuse rather than write afresh"
 * instruction this whole i386 self-hosting phase followed
 * (docs/kernel-arch-split-plan.md). musl's own libc calls (fork(),
 * wait4(), write()) are already portable; only the *build* (against
 * musl-i386 instead of musl-riscv64) differs, handled entirely by
 * this file's own Makefile rule.
 *
 * Deliberately avoids printf/malloc (this checkpoint doesn't touch
 * brk/mmap, which are still file-static globals shared across every
 * process -- see syscall_posix.c's own get_brk_current() comment --
 * so exercising them here would be testing a pre-existing, documented
 * simplification this checkpoint doesn't fix, not fork() itself);
 * write() and a tiny hand-rolled decimal formatter are enough to
 * prove what matters:
 *
 *   - `counter` is a real global, written to 1 *before* fork(). The
 *     child then overwrites its own copy to 42 and exits with status
 *     7; if fork() really gave it a copy-on-write *clone* of the
 *     parent's address space (mm/paging_common.c's paging_fork_cow(),
 *     shared with riscv64) rather than e.g. accidentally sharing it,
 *     the parent's own `counter` is untouched by that write -- still 1
 *     when the parent reads it back after wait4() returns.
 *   - wait4()'s reaped exit status must be exactly the child's real
 *     exit code (7), proving process_exit_current()'s exit_code and
 *     process_wait4()'s reaping are wired together correctly, not
 *     just "wait4() returns *something*".
 *
 * i386's own fork() reaches musl-i386's legacy SYS_fork (2), not
 * SYS_clone the way riscv64's musl build does -- see
 * syscall_posix.c's own sys_fork() comment -- but that's entirely
 * inside libc/kernel plumbing this test never has to know about. */
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
