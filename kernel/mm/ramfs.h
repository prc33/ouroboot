#ifndef RAMFS_H
#define RAMFS_H

/* checkpoint 8: the smallest possible real filesystem -- a fixed,
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
 * it open a few files", which a fixed table already does honestly. */

struct ramfs_file {
	const char *name; /* no leading slash */
	const unsigned char *data;
	unsigned long size;
};

/* Looks up `path` (a leading '/' is stripped if present -- this
 * ramfs has no subdirectories, so that's the only path normalization
 * that means anything) against the fixed table in mm/ramfs.c.
 * Returns 0 if not found. */
const struct ramfs_file *ramfs_lookup(const char *path);

#endif
