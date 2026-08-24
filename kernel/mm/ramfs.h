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

#endif
