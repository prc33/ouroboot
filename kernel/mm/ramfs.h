#ifndef RAMFS_H
#define RAMFS_H

/* A writable in-memory filesystem populated exclusively from an external tar
 * initrd. The kernel embeds no file contents. BusyBox applet aliases are the
 * sole fixed metadata: ramfs_lookup() resolves an applet basename to the
 * dynamically loaded `busybox` file, like a conventional symlink farm. */

struct ramfs_file {
	const char *name; /* no leading slash */
	const unsigned char *data;
	unsigned long size;
};

/* Resolves a BusyBox applet basename to the initrd-loaded `busybox` bytes.
 * Ordinary files use ramfs_dynamic_lookup() and retain their full paths. */
const struct ramfs_file *ramfs_lookup(const char *path);

/* The synthetic root entries are the BusyBox aliases. Real files loaded from
 * the initrd are enumerated by ramfs_dynamic_entry_* below. */
unsigned int ramfs_root_entry_count(void);
const char *ramfs_root_entry_name(unsigned int index); /* index must be < ramfs_root_entry_count() */
int ramfs_root_entry_is_symlink(unsigned int index);   /* every entry is an alias */

/* A real writable file, loaded or created at runtime. `data` is a physical
 * address that is also a kernel pointer through identity-mapped RAM. NULL/0 until
 * the first write actually needs backing storage: an empty file
 * (right after O_CREAT, nothing written yet) costs zero pages, same
 * as a real filesystem. */
struct ramfs_dynamic_file {
	int used;
	char name[128]; /* full normalized path (leading '/' stripped, everything
	                  * after kept as-is), NOT basename.
	                  * 128 is real headroom over the longest path this kernel
	                  * loads (musl's own arch/riscv64/bits/*.h.in
	                  * paths top out in the 30s), not a tight fit. */
	unsigned char *data;
	unsigned long size;     /* logical content length */
	unsigned long capacity; /* currently-allocated backing size, a multiple of PAGE_SIZE */
};

/* Full-path-matched, deliberately unlike the BusyBox alias lookup: a real
 * multi-directory build environment (musl's own header tree: `include/`, `arch/riscv64/`,
 * `arch/generic/`, searched via multiple -I flags with real override-by-
 * priority semantics) genuinely has *different* files sharing the same
 * basename (confirmed empirically: bits/fenv.h, errno.h, fcntl.h,
 * dirent.h, ioctl.h, and more, all present under more than one of those
 * directories with different content). Collapsing those to basename-only
 * would silently make the wrong one win. So a dynamic file's identity is
 * its full path (normalized: a single leading '/' stripped, so
 * "/tcc-src/tcc.c" and "tcc-src/tcc.c" name the same file, matching how sys_openat/execve
 * already treat a leading '/' as "the one root this ramfs has" rather
 * than something meaningfully different from no leading '/' at all) --
 * this ramfs still has no real directory *inodes* (no mkdir, no
 * per-directory readdir, no '.'/'..'), just path strings used as opaque
 * lookup keys, which is all a real `-I dir` + `#include "file.h"` build
 * actually needs (open()/stat() by constructed path, never readdir() on
 * one of these subdirectories). Returns 0 if no dynamic file by that
 * path currently exists. */
struct ramfs_dynamic_file *ramfs_dynamic_lookup(const char *path);

/* Finds an existing dynamic file by that name, or allocates a fresh
 * (empty, zero-capacity) slot if none exists -- the O_CREAT half of
 * openat(). Returns 0 only if every slot is already in use (a real,
 * if generous, fixed ceiling -- see mm/ramfs.c's own RAMFS_MAX_DYNAMIC_FILES). */
struct ramfs_dynamic_file *ramfs_dynamic_open_or_create(const char *path);

/* Forgets a dynamic file's content (size back to 0) without freeing
 * its backing capacity -- the O_TRUNC half of openat(), and (see
 * sys_openat's own comment) also what a real filesystem's rewrite-in-
 * place effectively does at the content level. Keeping the capacity
 * around rather than freeing-then-reallocating is the same "shrink is
 * accounting-only, no reclaim yet" simplification arch/riscv64_syscall.c's
 * sys_brk already documents for itself. */
void ramfs_dynamic_truncate(struct ramfs_dynamic_file *f);

/* Removes a dynamic file outright, freeing its backing pages. A no-op if no
 * dynamic file by that name exists; this matches how every caller treats
 * unlink()'s return value as best-effort
 * (see arch/riscv64_syscall.c's
 * own comment on tcc_write_elf_file's unlink-before-create pattern). */
void ramfs_dynamic_unlink(const char *path);

/* Writes `len` bytes from `src` into `f` starting at byte `offset`,
 * growing (and zero-filling any real gap left by a write past the
 * current end, same as a real sparse-file hole) as needed. Returns 0
 * on success, -1 on failure (pmm_alloc_contiguous() couldn't find
 * enough contiguous free RAM -- real ENOMEM, not a bug). */
int ramfs_dynamic_write(struct ramfs_dynamic_file *f, unsigned long offset, const unsigned char *src, unsigned long len);

/* Same index-based shape as ramfs_root_entry_count()/_name() above,
 * for the same reason (arch/riscv64_syscall.c's sys_getdents64 builds
 * dirent64 records for these too, appended after the BusyBox aliases,
 * so a freshly created file actually shows up in `ls`). */
unsigned int ramfs_dynamic_entry_count(void);
const char *ramfs_dynamic_entry_name(unsigned int index); /* index must be < ramfs_dynamic_entry_count() */

#endif
