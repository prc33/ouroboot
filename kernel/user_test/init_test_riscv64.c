/* checkpoint 9 test payload: the actual "run busybox ash" milestone.
 * Forks, has the child execve() into "ash" (resolved through
 * mm/ramfs.h's busybox multi-call table, not a separate binary) with
 * argv={"ash","/test.sh"} -- /test.sh (mm/ramfs.c) exercises both a
 * real ash builtin (true, pwd) and a real *external* command dispatch
 * through busybox's own argv[0] mechanism (echo -- confirmed via
 * shell/ash.c's own source that echo isn't a builtin in this build).
 * The parent wait4()s and checks the real reaped exit status (5,
 * test.sh's own explicit `exit 5`) -- proving the whole chain: ramfs
 * lookup -> execve -> real ash parsing+execution -> builtin dispatch
 * -> external command PATH search -> a *second* execve (echo) ->
 * *that* process's own exit -> ash's own exit -> this process's
 * wait4(). */
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
		char *argv[] = { "ash", "/test.sh", 0 };
		char *envp[] = { 0 };
		execve("ash", argv, envp);
		write_str("execve ash failed\n");
		_exit(97);
	}

	int status = 0;
	wait4(pid, &status, 0, 0);
	write_str("init test: ash_status=");
	write_int(WEXITSTATUS(status));
	write_str("\n");
	return 0;
}
