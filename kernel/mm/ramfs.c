#include "ramfs.h"
#include "pmm.h"
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

/* checkpoint 12: real writable files -- see ramfs.h's own comment on
 * struct ramfs_dynamic_file. A fixed table, same shape as files[]
 * above and for the same reason (no dynamic allocation of the
 * *table itself* -- only of each entry's own backing storage, via
 * mm/pmm.h's pmm_alloc_contiguous()); 16 is generous headroom for
 * what a single `tcc -o out.elf a.c b.c c.c`-style invocation
 * actually needs open for writing at once. */
#define RAMFS_MAX_DYNAMIC_FILES 16
static struct ramfs_dynamic_file dynamic_files[RAMFS_MAX_DYNAMIC_FILES];

static void name_copy(char *dst, const char *src, unsigned int cap) {
	unsigned int i = 0;
	while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
	dst[i] = 0;
}

struct ramfs_dynamic_file *ramfs_dynamic_lookup(const char *path) {
	const char *base = basename_of(path);
	for (unsigned int i = 0; i < RAMFS_MAX_DYNAMIC_FILES; i++)
		if (dynamic_files[i].used && streq(base, dynamic_files[i].name))
			return &dynamic_files[i];
	return 0;
}

struct ramfs_dynamic_file *ramfs_dynamic_open_or_create(const char *path) {
	struct ramfs_dynamic_file *existing = ramfs_dynamic_lookup(path);
	if (existing)
		return existing;
	const char *base = basename_of(path);
	for (unsigned int i = 0; i < RAMFS_MAX_DYNAMIC_FILES; i++) {
		if (!dynamic_files[i].used) {
			dynamic_files[i].used = 1;
			name_copy(dynamic_files[i].name, base, sizeof(dynamic_files[i].name));
			dynamic_files[i].data = 0;
			dynamic_files[i].size = 0;
			dynamic_files[i].capacity = 0;
			return &dynamic_files[i];
		}
	}
	return 0; /* every slot in use -- a real, if generous, fixed ceiling */
}

void ramfs_dynamic_truncate(struct ramfs_dynamic_file *f) {
	/* Content beyond `size` is never trusted to already be zero (see
	 * ramfs_dynamic_write's own comment on why it re-zeros a gap
	 * itself rather than relying on that) -- this really is just the
	 * one-line "forget the content" it looks like. */
	f->size = 0;
}

void ramfs_dynamic_unlink(const char *path) {
	struct ramfs_dynamic_file *f = ramfs_dynamic_lookup(path);
	if (!f)
		return; /* no matching dynamic file -- silently harmless, see this function's own header comment */
	if (f->data) {
		unsigned int pages = (unsigned int)(f->capacity / PAGE_SIZE);
		unsigned int base = (unsigned int)(unsigned long)f->data;
		for (unsigned int i = 0; i < pages; i++)
			pmm_free_page(base + i * PAGE_SIZE);
	}
	f->used = 0;
	f->data = 0;
	f->size = 0;
	f->capacity = 0;
}

/* Grows f's backing capacity to at least `needed` bytes if it isn't
 * already -- a fresh, larger contiguous run, the old content copied
 * over, the old pages freed one at a time. Doubling growth (starting
 * from one page) for amortized O(1) reallocations per byte written
 * overall, the same reasoning as any growable-array design; an empty
 * file (capacity 0) costs nothing until the first write actually
 * needs somewhere to go. Returns 0 on success, -1 if
 * pmm_alloc_contiguous() can't find enough contiguous free RAM (real
 * ENOMEM, not a bug -- see its own comment). */
static int ramfs_dynamic_grow(struct ramfs_dynamic_file *f, unsigned long needed) {
	if (needed <= f->capacity)
		return 0;
	unsigned long new_capacity = f->capacity ? f->capacity : PAGE_SIZE;
	while (new_capacity < needed)
		new_capacity *= 2;
	unsigned int new_pages = (unsigned int)(new_capacity / PAGE_SIZE);
	unsigned int new_base = pmm_alloc_contiguous(new_pages);
	if (!new_base)
		return -1;
	unsigned char *new_data = (unsigned char *)(unsigned long)new_base;
	unsigned long i;
	for (i = 0; i < f->size; i++)
		new_data[i] = f->data[i];
	for (i = f->size; i < new_capacity; i++)
		new_data[i] = 0; /* fresh pages -- real content, not garbage, from the first byte past size onward */
	if (f->data) {
		unsigned int old_pages = (unsigned int)(f->capacity / PAGE_SIZE);
		unsigned int old_base = (unsigned int)(unsigned long)f->data;
		for (unsigned int p = 0; p < old_pages; p++)
			pmm_free_page(old_base + p * PAGE_SIZE);
	}
	f->data = new_data;
	f->capacity = new_capacity;
	return 0;
}

int ramfs_dynamic_write(struct ramfs_dynamic_file *f, unsigned long offset, const unsigned char *src, unsigned long len) {
	if (len == 0)
		return 0;
	if (ramfs_dynamic_grow(f, offset + len) < 0)
		return -1;
	/* A write starting past the current logical size leaves a real
	 * gap -- a sparse-file hole, same POSIX semantics as a real
	 * lseek()-past-EOF-then-write(), which TCC's own ELF writer
	 * genuinely does (tccelf.c's tcc_write_elf_file() lays out
	 * sections at their real file offsets, not necessarily strictly
	 * increasing). ramfs_dynamic_grow() already zeroed any *newly
	 * allocated* capacity, but that's not sufficient by itself: if
	 * capacity already covered this offset from an earlier, larger
	 * write later ramfs_dynamic_truncate()'d back down, the bytes
	 * sitting there are stale old content, not zero -- so the gap is
	 * re-zeroed explicitly here regardless of whether grow() just ran. */
	if (offset > f->size)
		for (unsigned long i = f->size; i < offset; i++)
			f->data[i] = 0;
	for (unsigned long i = 0; i < len; i++)
		f->data[offset + i] = src[i];
	if (offset + len > f->size)
		f->size = offset + len;
	return 0;
}

unsigned int ramfs_dynamic_entry_count(void) {
	unsigned int n = 0;
	for (unsigned int i = 0; i < RAMFS_MAX_DYNAMIC_FILES; i++)
		if (dynamic_files[i].used)
			n++;
	return n;
}

const char *ramfs_dynamic_entry_name(unsigned int index) {
	unsigned int seen = 0;
	for (unsigned int i = 0; i < RAMFS_MAX_DYNAMIC_FILES; i++) {
		if (dynamic_files[i].used) {
			if (seen == index)
				return dynamic_files[i].name;
			seen++;
		}
	}
	return 0;
}
