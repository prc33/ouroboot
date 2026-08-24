#include "ramfs.h"
#include "../exec_target_elf_riscv64_payload.h"

/* "greeting": a plain data file (not an ELF), read by
 * user_test/exec_target_riscv64.c's own open()+read()+close() test --
 * proves the VFS syscalls work against ordinary file content, not
 * just executables. "exec_target": a real ELF, looked up *by name*
 * and run via execve() by user_test/proc_exec_test_riscv64.c -- the
 * same bytes as hello_elf_riscv64_payload.h's own payload mechanism,
 * just reachable through open()/execve() instead of being wired
 * directly into a process_create_from_elf() call. */
static const char greeting_data[] = "hello from ramfs\n";

static const struct ramfs_file files[] = {
	{ "greeting", (const unsigned char *)greeting_data, sizeof(greeting_data) - 1 },
	{ "exec_target", exec_target_elf_riscv64_payload, EXEC_TARGET_ELF_RISCV64_SIZE },
};
#define NUM_FILES (sizeof(files) / sizeof(files[0]))

static int streq(const char *a, const char *b) {
	while (*a && *a == *b) { a++; b++; }
	return *a == 0 && *b == 0;
}

const struct ramfs_file *ramfs_lookup(const char *path) {
	if (*path == '/')
		path++;
	for (unsigned int i = 0; i < NUM_FILES; i++)
		if (streq(path, files[i].name))
			return &files[i];
	return 0;
}
