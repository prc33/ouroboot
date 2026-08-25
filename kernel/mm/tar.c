#include "tar.h"
#include "ramfs.h"

#define TAR_BLOCK 512

/* USTAR header field offsets (POSIX.1-1988) -- confirmed against the
 * real, well-documented on-disk format, not guessed:
 *   0   name[100]     124  size[12] (octal ASCII)
 * 156   typeflag[1]  (see below)
 * everything else (mode/uid/gid/mtime/chksum/linkname/magic/version/
 * uname/gname/devmajor/devminor/prefix) this loader doesn't need --
 * this ramfs has no permissions/ownership/symlink model to feed them
 * into, same "hardcode what we actually use" reasoning as everywhere
 * else in this kernel. */
#define TAR_NAME_OFF 0
#define TAR_NAME_LEN 100
#define TAR_SIZE_OFF 124
#define TAR_SIZE_LEN 12
#define TAR_TYPEFLAG_OFF 156

/* Regular file, in either the pre-POSIX convention (a literal NUL
 * byte, never written but still valid to read per the spec) or the
 * POSIX one (ASCII '0') -- real tar writers (GNU tar, bsdtar) both
 * always write the POSIX form, but reading a NUL as "also means
 * regular file" costs nothing and matches the spec exactly. */
#define TAR_TYPE_REGULAR '0'

static unsigned long oct_to_ulong(const char *s, unsigned int maxlen) {
	unsigned long v = 0;
	for (unsigned int i = 0; i < maxlen && s[i] >= '0' && s[i] <= '7'; i++)
		v = v * 8 + (unsigned long)(s[i] - '0');
	return v;
}

static int block_all_zero(const unsigned char *block) {
	for (unsigned int i = 0; i < TAR_BLOCK; i++)
		if (block[i])
			return 0;
	return 1;
}

static unsigned long round_up_block(unsigned long n) {
	return (n + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
}

unsigned int tar_load_initrd(const unsigned char *data, unsigned long max_size) {
	unsigned int count = 0;
	unsigned long off = 0;

	while (off + TAR_BLOCK <= max_size) {
		const unsigned char *hdr = data + off;
		if (block_all_zero(hdr))
			break; /* real tar's own end-of-archive marker (two zero
			        * blocks in a row, but one is already enough to
			        * know there's nothing real left to read) */

		char typeflag = (char)hdr[TAR_TYPEFLAG_OFF];
		unsigned long size = oct_to_ulong((const char *)(hdr + TAR_SIZE_OFF), TAR_SIZE_LEN);
		off += TAR_BLOCK;

		if (typeflag == TAR_TYPE_REGULAR || typeflag == 0) {
			/* name[100] isn't guaranteed NUL-terminated if it's
			 * exactly 100 bytes long (the spec allows that) -- copy
			 * into a local, always-terminated buffer before treating
			 * it as a C string, same defensive convention
			 * arch/riscv64_syscall.c's copy_path_from_user() uses for
			 * user-supplied paths. */
			char name[TAR_NAME_LEN + 1];
			unsigned int i;
			for (i = 0; i < TAR_NAME_LEN && hdr[TAR_NAME_OFF + i]; i++)
				name[i] = (char)hdr[TAR_NAME_OFF + i];
			name[i] = 0;

			if (name[0] && off + size <= max_size) {
				struct ramfs_dynamic_file *f = ramfs_dynamic_open_or_create(name);
				if (f) {
					ramfs_dynamic_truncate(f); /* in case the same archive is ever loaded twice */
					if (ramfs_dynamic_write(f, 0, data + off, size) == 0)
						count++;
				}
			}
		}
		/* Skip this entry's data (block-padded, regardless of
		 * typeflag -- directories/symlinks/etc. get skipped the same
		 * way, no separate case needed since none of them have
		 * meaningful `size` data to preserve here anyway). */
		off += round_up_block(size);
	}
	return count;
}
