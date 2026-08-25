#include "ramfs.h"
#include "../exec_target_elf_riscv64_payload.h"
#include "../busybox_elf_riscv64_payload.h"

/* "greeting": plain data (not an ELF), read by
 * user_test/exec_target_riscv64.c's own open()+read()+close() test.
 * "exec_target": a real ELF, looked up *by name* and run via
 * execve() by user_test/proc_exec_test_riscv64.c. "test.sh": an ash
 * script (plain data, not an ELF -- ash itself is what makes it
 * executable, the same way a real #!-less shell script only ever
 * runs via `sh script` or `. script`, never execve()'d directly),
 * exercising both an ash builtin (true, pwd) and a real external
 * command dispatched through busybox's own multi-call mechanism
 * (echo -- confirmed via shell/ash.c's own source that echo is *not*
 * a builtin in this build, so this genuinely tests external exec,
 * not just parsing). */
static const char greeting_data[] = "hello from ramfs\n";
static const char test_script_data[] =
	"echo hello from ash\n"
	"pwd\n"
	"true && echo builtin ok\n"
	"exit 5\n";

/* Real busybox (user_test/busybox_riscv64.elf), embedded unmodified --
 * the exact binary demo/build-busybox-riscv64.sh builds and smoke-
 * tests, not a fork or a rebuild for this purpose. Looked up under
 * every applet name it actually supports, so ash's real PATH search
 * (bb_default_path, tried in order: /sbin, /usr/sbin, /usr/local/sbin,
 * /bin, /usr/bin, /usr/local/bin) finds *a* match under whichever
 * prefix it tries -- basename-only matching (ramfs_lookup, this
 * file's own comment) makes the directory prefix irrelevant, exactly
 * mirroring a real multi-call busybox install's symlink farm without
 * this ramfs needing to model directories or symlinks at all.
 *
 * This exact list -- not a guess, not "every applet busybox could
 * support" -- is copied from demo/build-busybox-riscv64.sh's own
 * `for opt in ASH GUNZIP GZIP TAR ...` loop, the actual build
 * configuration that produced the embedded binary above; anything not
 * in that list genuinely isn't compiled into this binary and
 * wouldn't work even if listed here. "busybox" itself is included
 * too, for `execve("/bin/busybox", ...)`-style invocations, though
 * running it with no applet argv[0] match doesn't print the usual
 * help text in this minimal build (confirmed: this build has no
 * CONFIG_FEATURE_INSTALLER-equivalent "show applet list" fallback --
 * an honest gap in busybox's own build, not something to fake here). */
static const char *const busybox_applets[] = {
	"busybox", "ash",
	"gunzip", "gzip", "tar", "basename", "cat", "chmod", "chown", "cp",
	"cut", "dirname", "echo", "env", "expr", "false", "head", "ln", "ls",
	"mkdir", "mv", "printf", "pwd", "rm", "rmdir", "sleep", "sort", "tail",
	"test", "touch", "true", "uniq", "wc", "which", "sed", "find", "grep",
	"mount", "umount", "kill", "ps",
};
#define NUM_APPLETS (sizeof(busybox_applets) / sizeof(busybox_applets[0]))

static const struct ramfs_file files[] = {
	{ "greeting", (const unsigned char *)greeting_data, sizeof(greeting_data) - 1 },
	{ "exec_target", exec_target_elf_riscv64_payload, EXEC_TARGET_ELF_RISCV64_SIZE },
	{ "test.sh", (const unsigned char *)test_script_data, sizeof(test_script_data) - 1 },
};
#define NUM_FILES (sizeof(files) / sizeof(files[0]))

/* What every entry in busybox_applets[] resolves to -- always the
 * same blob regardless of which alias matched; the child's own
 * argv[0] (set by whoever called execve(), not anything here) is
 * what picks the applet, matching real multi-call dispatch. */
static const struct ramfs_file busybox_entry = {
	"busybox", busybox_elf_riscv64_payload, BUSYBOX_ELF_RISCV64_SIZE
};

static int streq(const char *a, const char *b) {
	while (*a && *a == *b) { a++; b++; }
	return *a == 0 && *b == 0;
}

/* Last path component -- "/usr/bin/echo" -> "echo", "greeting" ->
 * "greeting" unchanged (no '/' at all). */
static const char *basename_of(const char *path) {
	const char *base = path;
	for (const char *p = path; *p; p++)
		if (*p == '/')
			base = p + 1;
	return base;
}

const struct ramfs_file *ramfs_lookup(const char *path) {
	const char *base = basename_of(path);

	for (unsigned int i = 0; i < NUM_FILES; i++)
		if (streq(base, files[i].name))
			return &files[i];

	for (unsigned int i = 0; i < NUM_APPLETS; i++)
		if (streq(base, busybox_applets[i]))
			return &busybox_entry;

	return 0;
}

unsigned int ramfs_root_entry_count(void) {
	return NUM_FILES + NUM_APPLETS;
}

const char *ramfs_root_entry_name(unsigned int index) {
	if (index < NUM_FILES)
		return files[index].name;
	return busybox_applets[index - NUM_FILES];
}

int ramfs_root_entry_is_symlink(unsigned int index) {
	/* index == NUM_FILES is busybox_applets[0] == "busybox" itself,
	 * the real file; every applet index after that is an alias. */
	return index > NUM_FILES;
}
