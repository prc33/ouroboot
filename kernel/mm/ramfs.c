#include "ramfs.h"
#include "pmm.h"
/* Names provided by the BusyBox binary in the external initrd. This is only
 * symlink-farm metadata: no file bytes are compiled into the kernel. */
static const char *const busybox_applets[] = {
	"ash",
	"gunzip", "gzip", "tar", "basename", "cat", "chmod", "chown", "cp",
	"cut", "dirname", "echo", "env", "expr", "false", "head", "ln", "ls",
	"mkdir", "mv", "printf", "pwd", "rm", "rmdir", "sleep", "sort", "tail",
	"test", "touch", "true", "uniq", "wc", "which", "sed", "find", "grep",
	"mount", "umount", "kill", "ps",
};
#define NUM_APPLETS (sizeof(busybox_applets) / sizeof(busybox_applets[0]))

static int streq(const char *a, const char *b) {
	while (*a && *a == *b) { a++; b++; }
	return *a == 0 && *b == 0;
}

/* Last path component, used only to recognize BusyBox applet aliases. */
static const char *basename_of(const char *path) {
	const char *base = path;
	for (const char *p = path; *p; p++)
		if (*p == '/')
			base = p + 1;
	return base;
}

/* Strips a leading root slash and any leading "./" components --
 * "/tcc-src/tcc.c" and "tcc-src/tcc.c", or "./wtest" and "wtest", name the same
 * dynamic file (see ramfs.h's own comment on struct ramfs_dynamic_file),
 * matching how sys_openat/execve treat this ramfs as having one root.
 * Other path components are kept verbatim. */
static const char *normalize_path(const char *path) {
	if (path[0] == '/')
		path++;
	while (path[0] == '.' && path[1] == '/')
		path += 2;
	return path;
}

const struct ramfs_file *ramfs_lookup(const char *path) {
	const char *base = basename_of(path);
	int is_busybox = streq(base, "busybox");

	for (unsigned int i = 0; i < NUM_APPLETS && !is_busybox; i++)
		is_busybox = streq(base, busybox_applets[i]);
	if (is_busybox) {
		struct ramfs_dynamic_file *busybox = ramfs_dynamic_lookup("busybox");
		static struct ramfs_file alias;
		if (!busybox)
			return 0;
		alias.name = base;
		alias.data = busybox->data;
		alias.size = busybox->size;
		return &alias;
	}

	return 0;
}

unsigned int ramfs_root_entry_count(void) {
	return NUM_APPLETS;
}

const char *ramfs_root_entry_name(unsigned int index) {
	return busybox_applets[index];
}

int ramfs_root_entry_is_symlink(unsigned int index) {
	(void)index;
	return 1;
}

/* The file table has fixed capacity; file contents use dynamically allocated
 * backing storage.
 *
 * checkpoint 14: bumped from 16 to 512 -- a real self-hosting TCC
 * build needs far more than "one invocation's worth of output files"
 * open at once, since every input file the build reads also lives
 * here now (the tar-loaded initrd creates a dynamic file per entry,
 * mm/tar.c's own tar_load_initrd()): TCC's own ~20 source/header
 * files, musl-riscv64's header tree (~230 files, measured directly --
 * `find include arch/riscv64 arch/generic obj/include -type f | wc -l`),
 * crt1.o/crti.o/crtn.o/libc.a/libtcc1.a, the prebuilt riscv64 tcc
 * binary that does the compiling, plus whatever the build itself
 * writes out. 512 is real headroom above that measured ~260, not a
 * guess padded for its own sake. */
#define RAMFS_MAX_DYNAMIC_FILES 512
static struct ramfs_dynamic_file dynamic_files[RAMFS_MAX_DYNAMIC_FILES];

static void name_copy(char *dst, const char *src, unsigned int cap) {
	unsigned int i = 0;
	while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
	dst[i] = 0;
}

struct ramfs_dynamic_file *ramfs_dynamic_lookup(const char *path) {
	const char *norm = normalize_path(path);
	for (unsigned int i = 0; i < RAMFS_MAX_DYNAMIC_FILES; i++)
		if (dynamic_files[i].used && streq(norm, dynamic_files[i].name))
			return &dynamic_files[i];
	return 0;
}

struct ramfs_dynamic_file *ramfs_dynamic_open_or_create(const char *path) {
	struct ramfs_dynamic_file *existing = ramfs_dynamic_lookup(path);
	if (existing)
		return existing;
	const char *norm = normalize_path(path);
	for (unsigned int i = 0; i < RAMFS_MAX_DYNAMIC_FILES; i++) {
		if (!dynamic_files[i].used) {
			dynamic_files[i].used = 1;
			name_copy(dynamic_files[i].name, norm, sizeof(dynamic_files[i].name));
			dynamic_files[i].data = 0;
			dynamic_files[i].size = 0;
			dynamic_files[i].capacity = 0;
			return &dynamic_files[i];
		}
	}
	return 0; /* every slot in use -- a real, if generous, fixed ceiling */
}

int ramfs_dynamic_load(const char *path, const unsigned char *data, unsigned long size) {
	struct ramfs_dynamic_file *f = ramfs_dynamic_open_or_create(path);
	if (!f)
		return -1;
	f->data = (unsigned char *)data;
	f->size = size;
	f->capacity = 0; /* borrowed initrd storage; grow() makes it writable */
	return 0;
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
	if (f->capacity) {
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
	if (f->capacity) {
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
