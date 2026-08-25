#ifndef RAMFS_H
#define RAMFS_H

/* checkpoint 8/9: the smallest possible real filesystem -- a fixed,
 * read-only, flat (no directories) table of embedded files, built
 * into the kernel image the same way every other test payload is
 * (mm/ramfs.c's own file table). "Real" in the sense every payload
 * in this kernel already is: actual bytes an actual open()+read()+
 * close() from a real musl+TCC binary actually reads, not simulated
 * -- just not backed by an actual block device or a writable/dynamic
 * namespace yet. That's future scope (a real VFS layer, a real ramfs
 * you can create files in, eventually a real block device), tracked
 * but deliberately out of reach of this checkpoint: everything this
 * kernel needs from a filesystem right now is "load busybox and let
 * it open a few files", which a fixed table already does honestly.
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

#endif
