#ifndef TAR_H
#define TAR_H

/* checkpoint 13: a real (uncompressed, USTAR) tar archive, already
 * sitting in memory -- loaded at a fixed physical address
 * (arch/riscv64_memmap.h's RV64_INITRD_BASE) by whatever actually
 * booted the kernel, *not* baked into kernel.elf the way every other
 * embedded payload in this kernel is (mm/ramfs.c's own files[]/
 * busybox_elf_riscv64_payload.h). Every regular file entry becomes a
 * real, immediately readable/writable/listable ramfs entry (mm/ramfs.h's
 * own checkpoint 12 dynamic files) -- the same mechanism a running
 * program creating a file with open()+write() uses, not a separate
 * read-only fixed table. Directories/symlinks/etc are silently
 * skipped -- this ramfs is flat (basename-matched, mm/ramfs.c's own
 * comment), so a tar's own directory structure doesn't need modeling,
 * just each real file's content.
 *
 * Uncompressed tar, not zip: USTAR's own format is a flat, forward-
 * only stream of fixed 512-byte headers + block-padded data, parseable
 * with zero dependencies (no inflate, no seeking to a trailing central
 * directory the way zip needs) -- matches how every other from-scratch
 * parser in this project is built (arch/riscv64_syscall.c's
 * fill_dirent64(), for one). */

/* Parses the tar archive at `data`, stopping at its own end-of-archive
 * marker (two consecutive all-zero 512-byte blocks -- real tar's own
 * self-terminating convention, so the *exact* archive size never
 * needs to be known or passed in) or after `max_size` bytes, whichever
 * comes first -- `max_size` is a safety bound against scanning into
 * uninitialized memory if no real archive was actually loaded (which
 * this function then correctly, harmlessly treats as "zero bytes read,
 * zero files loaded" -- every real tar starts with a real header, and
 * unloaded RAM reads as zero, so the very first end-of-archive check
 * fires immediately). Returns the number of regular files loaded. */
unsigned int tar_load_initrd(const unsigned char *data, unsigned long max_size);

#endif
