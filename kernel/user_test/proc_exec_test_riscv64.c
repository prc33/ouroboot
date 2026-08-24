/* checkpoint 8 test payload: real fork()+execve()+wait4(), same real
 * musl+TCC pipeline as every other payload. Forks, has the child
 * execve() into exec_target_riscv64.c (a completely different real
 * binary, in a brand new address space -- not just fork()'s COW
 * clone of this one), and has the parent wait4() and check the
 * reaped exit status matches exec_target's real return value (42) --
 * proving the whole fork -> execve -> real exit code round trip, not
 * just that *some* process eventually exited. */
#include <unistd.h>
#include <sys/wait.h>

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
	pid_t pid = fork();
	if (pid < 0) {
		write_str("fork failed\n");
		return 1;
	}
	if (pid == 0) {
		char *argv[] = { "exec_target", "execved_ok", 0 };
		char *envp[] = { 0 };
		execve("/exec_target", argv, envp);
		write_str("execve failed\n");
		_exit(97);
	}

	int status = 0;
	wait4(pid, &status, 0, 0);
	write_str("exec test: child_status=");
	write_int(WEXITSTATUS(status));
	write_str("\n");
	return 0;
}
