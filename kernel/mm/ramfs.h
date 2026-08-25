#ifndef RAMFS_H
#define RAMFS_H

/* checkpoint 8/9: the smallest possible real filesystem -- a fixed,
 * read-only, flat (no directories) table of embedded files, built
 * into the kernel image the same way every other test payload is
 * (mm/ramfs.c's own file table). "Real" in the sense every payload
 * in this kernel already is: actual bytes an actual open()+read()+
 * close() from a real musl+TCC binary actually reads, not simulated
 * -- just not backed by an actual block device.
 *
 * checkpoint 12 addition: this table (`files[]`) is still read-only
 * and compile-time-fixed, but it's no longer the *only* thing a path
 * can resolve to -- see struct ramfs_dynamic_file and its own
 * accessors below for the writable half, which is what makes a real
 * `tcc -o out.elf in.c` (creating and writing a new file at runtime)
 * possible at all.
 *
 * checkpoint 9 addition: real busybox (user_test/busybox_riscv64.elf,
 * the exact binary demo/build-busybox-riscv64.sh already builds and
 * smoke-tests, embedded unmodified) is looked up under *every* name
 * it's a real multi-call applet for -- see mm/ramfs.c's own comment
 * for why that's not a guess but confirmed against both this
 * project's own build script (the definitive list of which applets
 * this exact binary supports) and busybox's real argv[0] dispatch
 * convention (confirmed empirically: `./busybox echo hello` prints
 * "hello", the same binary recognizing its own applet name). */

struct ramfs_file {
	const char *name; /* no leading slash */
	const unsigned char *data;
	unsigned long size;
};

/* Looks up `path` against the fixed table in mm/ramfs.c, matching on
 * *basename* (the part after the last '/', or the whole thing if
 * there's no '/') -- this ramfs has no real subdirectories, so any
 * directory prefix a caller supplies (ash's own PATH search tries
 * several: /usr/bin, /bin, etc.) is accepted and ignored rather than
 * treated as part of the identity, the same way a real multi-call
 * busybox install (one binary, symlinked under every applet name in
 * every PATH directory) makes any of those paths resolve to the same
 * file. Returns 0 if no name/basename in the table matches. */
const struct ramfs_file *ramfs_lookup(const char *path);

/* checkpoint 11: real ls/opendir/readdir -- "." and "/" both mean the
 * one flat root directory (arch/riscv64_syscall.c's sys_newfstatat's
 * own comment), and its contents are exactly what ramfs_lookup() can
 * actually resolve: mm/ramfs.c's own files[] (real files) plus every
 * name in busybox_applets[] -- applets[0] itself ("busybox") is the
 * real file every other name in that list is a symlink to on a real
 * multi-call install, reported the same way here (see
 * ramfs_root_entry_is_symlink()). Index-based rather than returning
 * anything dirent-shaped, so this header doesn't need to know
 * struct dirent64's exact (Linux-ABI, not C-standard) layout --
 * arch/riscv64_syscall.c's sys_getdents64 builds real records from
 * this. */
unsigned int ramfs_root_entry_count(void);
const char *ramfs_root_entry_name(unsigned int index); /* index must be < ramfs_root_entry_count() */
int ramfs_root_entry_is_symlink(unsigned int index);   /* 1 for every busybox alias but the first */

/* checkpoint 12: a real writable file -- created and grown at
 * runtime (mm/pmm.h's pmm_alloc_contiguous(), not compile-time
 * embedded data), everything the fixed files[] table above
 * deliberately isn't. `data` is a physical address that's also a
 * valid kernel-mode pointer without any extra mapping step, the same
 * "identity-mapped RAM" property mm/ramfs.c's own fixed entries
 * already lean on -- see paging_init()'s own comment. NULL/0 until
 * the first write actually needs backing storage: an empty file
 * (right after O_CREAT, nothing written yet) costs zero pages, same
 * as a real filesystem. */
struct ramfs_dynamic_file {
	int used;
	char name[128]; /* full normalized path (leading '/' stripped, everything
	                  * after kept as-is), NOT basename -- see ramfs_dynamic_lookup's
	                  * own comment for why this differs from the fixed table.
	                  * 128 is real headroom over the longest path this kernel
	                  * actually embeds (musl's own arch/riscv64/bits/*.h.in
	                  * paths top out in the 30s), not a tight fit. */
	unsigned char *data;
	unsigned long size;     /* logical content length */
	unsigned long capacity; /* currently-allocated backing size, a multiple of PAGE_SIZE */
};

/* checkpoint 14: full-path-matched, deliberately *not* basename-matched
 * like ramfs_lookup() above. The fixed table's basename matching is
 * correct there because it's modeling a symlink farm (every busybox
 * applet name really does resolve to the same one binary, regardless
 * of which PATH directory found it) -- but a real multi-directory
 * build environment (musl's own header tree: `include/`, `arch/riscv64/`,
 * `arch/generic/`, searched via multiple -I flags with real override-by-
 * priority semantics) genuinely has *different* files sharing the same
 * basename (confirmed empirically: bits/fenv.h, errno.h, fcntl.h,
 * dirent.h, ioctl.h, and more, all present under more than one of those
 * directories with different content). Collapsing those to basename-only
 * would silently make the wrong one win. So a dynamic file's identity is
 * its full path (normalized: a single leading '/' stripped, so "/tcc/tcc.c"
 * and "tcc/tcc.c" name the same file, matching how sys_openat/execve
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

/* Removes a dynamic file outright, freeing its backing pages -- the
 * real unlink(). A no-op (not an error) if no dynamic file by that
 * name exists; mirrors the fixed files[]/busybox_applets[] table,
 * which can never be unlinked at all (real ROM, no backing store to
 * free) the same way a real filesystem's read-only mount would
 * reject it -- except this kernel doesn't need to reject it, since
 * "no matching dynamic file" already makes it silently harmless,
 * matching how every caller in this codebase treats unlink()'s
 * return value as best-effort anyway (see arch/riscv64_syscall.c's
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
 * dirent64 records for these too, appended after the fixed-table
 * entries, so a freshly created file actually shows up in `ls`). */
unsigned int ramfs_dynamic_entry_count(void);
const char *ramfs_dynamic_entry_name(unsigned int index); /* index must be < ramfs_dynamic_entry_count() */

#endif
