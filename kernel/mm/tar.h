#ifndef TAR_H
#define TAR_H

/* checkpoint 13: a real (uncompressed, USTAR) tar archive, already
 * sitting in memory -- loaded at a fixed physical address
 * (arch/risc/riscv64_memmap.h's RV64_INITRD_BASE) by whatever actually
 * booted the kernel; no filesystem contents are baked into kernel.elf.
 * Every regular file entry becomes a
 * immediately readable/listable ramfs entry. Its bytes remain in the initrd
 * until the first write, when the normal writable-file path copies them.
 * Explicit tar directory records and symlinks are skipped. Directories are
 * inferred from regular-file path prefixes and support stat, open/readdir,
 * chdir, getcwd, relative paths, and directory-relative openat.
 *
 * Uncompressed tar, not zip: USTAR's own format is a flat, forward-
 * only stream of fixed 512-byte headers + block-padded data, parseable
 * with zero dependencies (no inflate, no seeking to a trailing central
 * directory the way zip needs) -- matches how every other from-scratch
 * parser in this project is built (arch/risc/riscv64_syscall.c's
 * fill_dirent64(), for one). */

/* Parses the tar archive at `data`, stopping at its own end-of-archive
 * marker (two consecutive all-zero 512-byte blocks -- real tar's own
 * self-terminating convention, so the *exact* archive size never
 * needs to be known or passed in) or after `max_size` bytes, whichever
 * comes first -- `max_size` is a safety bound against scanning into
 * invalid memory. Returns the number of regular files loaded; the kernel
 * rejects zero because an explicit initrd is required. */
unsigned int tar_load_initrd(const unsigned char *data, unsigned long max_size);

#endif
