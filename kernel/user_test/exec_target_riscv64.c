/* checkpoint 8 test payload: what proc_exec_test_riscv64.c's child
 * execve()s into. Proves three things at once: argv really did
 * survive execve() (not just fork()'s COW, which this doesn't touch
 * at all -- this is a brand new address space), and open()+read()+
 * close() against mm/ramfs.h's embedded "/greeting" file work end to
 * end. */
#include <fcntl.h>
#include <unistd.h>

static void write_str(const char *s) {
	int n = 0;
	while (s[n]) n++;
	write(1, s, n);
}

int main(int argc, char **argv) {
	if (argc > 1) {
		write_str("argv1=");
		write_str(argv[1]);
		write_str("\n");
	}

	int fd = open("/greeting", O_RDONLY);
	if (fd < 0) {
		write_str("open failed\n");
		return 99;
	}
	char buf[64];
	int n = read(fd, buf, sizeof(buf));
	if (n > 0)
		write(1, buf, n);
	close(fd);
	return 42;
}
