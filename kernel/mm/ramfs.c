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
 * guess padded for its own sake. The source-build closure later raised this
 * to 8192: musl and BusyBox together contain several thousand regular files,
 * and their object files must coexist with those inputs while building. */
#define RAMFS_MAX_DYNAMIC_FILES 8192
static struct ramfs_dynamic_file dynamic_files[RAMFS_MAX_DYNAMIC_FILES];
static unsigned int dynamic_high_water;
/* Index-plus-one chains: zero is the natural BSS-initialized null value. */
static unsigned short dynamic_hash[RAMFS_MAX_DYNAMIC_FILES];
static unsigned short dynamic_next[RAMFS_MAX_DYNAMIC_FILES];
#define DIR_CACHE_ENTRIES 1024
static char dir_cache_path[128];
static char dir_cache_names[DIR_CACHE_ENTRIES][128];
static unsigned char dir_cache_directory[DIR_CACHE_ENTRIES];
static unsigned char dir_cache_symlink[DIR_CACHE_ENTRIES];
static unsigned int dir_cache_count;
static int dir_cache_valid;

static unsigned int name_hash(const char *name) {
	unsigned int h = 2166136261U;
	while (*name) { h ^= (unsigned char)*name++; h *= 16777619U; }
	return h & (RAMFS_MAX_DYNAMIC_FILES - 1);
}

static void name_copy(char *dst, const char *src, unsigned int cap) {
	unsigned int i = 0;
	while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
	dst[i] = 0;
}

struct ramfs_dynamic_file *ramfs_dynamic_lookup(const char *path) {
	const char *norm = normalize_path(path);
	for (unsigned int link = dynamic_hash[name_hash(norm)]; link; link = dynamic_next[link - 1]) {
		unsigned int i = link - 1;
		if (dynamic_files[i].used && streq(norm, dynamic_files[i].name))
			return &dynamic_files[i];
	}
	return 0;
}

static void hash_insert(unsigned int index) {
	unsigned int bucket = name_hash(dynamic_files[index].name);
	dynamic_next[index] = dynamic_hash[bucket];
	dynamic_hash[bucket] = index + 1;
}

struct ramfs_dynamic_file *ramfs_dynamic_open_or_create(const char *path) {
	struct ramfs_dynamic_file *existing = ramfs_dynamic_lookup(path);
	if (existing)
		return existing;
	const char *norm = normalize_path(path);
	for (unsigned int i = 0; i < dynamic_high_water; i++) {
		if (!dynamic_files[i].used) {
			dir_cache_valid = 0;
			dynamic_files[i].used = 1;
			name_copy(dynamic_files[i].name, norm, sizeof(dynamic_files[i].name));
			dynamic_files[i].data = 0;
			dynamic_files[i].size = 0;
			dynamic_files[i].capacity = 0;
			hash_insert(i);
			return &dynamic_files[i];
		}
	}
	if (dynamic_high_water < RAMFS_MAX_DYNAMIC_FILES) {
		dir_cache_valid = 0;
		struct ramfs_dynamic_file *f = &dynamic_files[dynamic_high_water++];
		f->used = 1;
		name_copy(f->name, norm, sizeof(f->name));
		f->data = 0;
		f->size = f->capacity = 0;
		hash_insert(dynamic_high_water - 1);
		return f;
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
	unsigned int index = (unsigned int)(f - dynamic_files);
	unsigned int bucket = name_hash(f->name);
	unsigned short *link = &dynamic_hash[bucket];
	while (*link && *link - 1 != index) link = &dynamic_next[*link - 1];
	if (*link) *link = dynamic_next[index];
	dir_cache_valid = 0;
	dynamic_next[index] = 0;
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
 * needs somewhere to go. Capacity beyond the logical size is left
 * uninitialized because it is unobservable; ramfs_dynamic_write() zeros any
 * sparse gap at the moment it becomes part of the file. Returns 0 on success, -1 if
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

static int prefix(const char *s, const char *p) {
	while (*p && *s == *p) { s++; p++; }
	return *p == 0;
}

static int child_name(const char *dir, const char *path, char *out,
	unsigned int capacity, int *directory) {
	const char *p = path;
	if (dir[0]) {
		unsigned int n = 0;
		if (!prefix(path, dir)) return 0;
		while (dir[n]) n++;
		p += n;
		if (*p++ != '/') return 0;
	}
	if (!*p) return 0;
	unsigned int n = 0;
	while (p[n] && p[n] != '/') n++;
	if (n + 1 > capacity) return 0;
	for (unsigned int i = 0; i < n; i++) out[i] = p[i];
	out[n] = 0;
	*directory = p[n] == '/';
	return 1;
}

static int raw_child(const char *dir, unsigned int raw, char *name,
	unsigned int capacity, int *directory, int *symlink) {
	if (raw < NUM_APPLETS) {
		if (!dir[0]) {
			name_copy(name, "bin", capacity);
			*directory = 1;
			*symlink = 0;
			return 1;
		}
		if (streq(dir, "bin")) {
			name_copy(name, busybox_applets[raw], capacity);
			*directory = 0;
			*symlink = 1;
			return 1;
		}
		return 0;
	}
	unsigned int slot = raw - NUM_APPLETS;
	if (slot >= RAMFS_MAX_DYNAMIC_FILES || !dynamic_files[slot].used) return 0;
	*symlink = 0;
	return child_name(dir, dynamic_files[slot].name, name, capacity, directory);
}

int ramfs_dir_entry(const char *dir, unsigned int index, char *name,
	unsigned int capacity, int *directory, int *symlink) {
	if (!dir_cache_valid || !streq(dir, dir_cache_path)) {
		name_copy(dir_cache_path, dir, sizeof(dir_cache_path));
		dir_cache_count = 0;
		unsigned int raw_count = dynamic_high_water + NUM_APPLETS;
		for (unsigned int raw = 0; raw < raw_count && dir_cache_count < DIR_CACHE_ENTRIES; raw++) {
			char candidate[128];
			int candidate_dir, candidate_link, duplicate = 0;
			if (!raw_child(dir, raw, candidate, sizeof(candidate), &candidate_dir, &candidate_link))
				continue;
			for (unsigned int earlier = 0; earlier < dir_cache_count; earlier++)
				if (streq(candidate, dir_cache_names[earlier])) { duplicate = 1; break; }
			if (duplicate) continue;
			name_copy(dir_cache_names[dir_cache_count], candidate, 128);
			dir_cache_directory[dir_cache_count] = candidate_dir;
			dir_cache_symlink[dir_cache_count] = candidate_link;
			dir_cache_count++;
		}
		dir_cache_valid = 1;
	}
	if (index >= dir_cache_count) return 0;
	name_copy(name, dir_cache_names[index], capacity);
	*directory = dir_cache_directory[index];
	*symlink = dir_cache_symlink[index];
	return 1;
}

int ramfs_is_dir(const char *dir) {
	const char *norm = normalize_path(dir);
	if (!norm[0] || streq(norm, "bin")) return 1;
	unsigned int n = 0;
	while (norm[n]) n++;
	for (unsigned int i = 0; i < dynamic_high_water; i++) {
		const char *path = dynamic_files[i].name;
		if (!dynamic_files[i].used) continue;
		unsigned int j = 0;
		while (j < n && path[j] == norm[j]) j++;
		if (j == n && path[j] == '/') return 1;
	}
	return 0;
}
