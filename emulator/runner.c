/* Native command-line front end for the RV64 emulator. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rv64.c"

static u64 elfint(const u8 *p, u32 n)
{
    u64 value = 0;
    while (n--) value = value << 8 | p[n];
    return value;
}
#define elf16(p) ((u16)elfint(p, 2))
#define elf32(p) ((u32)elfint(p, 4))
#define elf64(p) elfint(p, 8)

static u64 load_elf(const char *path)
{
    FILE *file = fopen(path, "rb");
    u8 header[64], program[56];
    u64 phoff, offset, address, filesz, memsz, entry;
    u32 i, phnum, phsize, entry_ok = 0;

    if (!file || fread(header, 1, sizeof header, file) != sizeof header ||
        elf32(header) != 0x464c457f || header[4] != 2 || header[5] != 1 ||
        elf16(header + 18) != 243) {
        fprintf(stderr, "%s: expected a 64-bit little-endian RISC-V ELF file\n", path);
        exit(1);
    }
    phoff = elf64(header + 32);
    entry = elf64(header + 24);
    phsize = elf16(header + 54);
    phnum = elf16(header + 56);
    if (phsize < sizeof program) {
        fprintf(stderr, "%s: bad program headers\n", path);
        exit(1);
    }

    for (i = 0; i < phnum; ++i) {
        if (fseek(file, (long)(phoff + (u64)i * phsize), SEEK_SET) ||
            fread(program, 1, sizeof program, file) != sizeof program)
            goto bad;
        if (elf32(program) != 1) continue;
        offset = elf64(program + 8);
        address = elf64(program + 16);
        filesz = elf64(program + 32);
        memsz = elf64(program + 40);
        if (address < RAM_BASE || address - RAM_BASE > RAM_SIZE ||
            memsz > RAM_SIZE - (address - RAM_BASE) || filesz > memsz ||
            offset > 0x7fffffffULL ||
            fseek(file, (long)offset, SEEK_SET) ||
            fread(ram + (u32)(address - RAM_BASE), 1, (size_t)filesz, file) != filesz)
            goto bad;
        memset(ram + (u32)(address - RAM_BASE + filesz), 0, (size_t)(memsz - filesz));
        if ((elf32(program + 4) & 1) && entry >= address && entry - address < memsz)
            entry_ok = 1;
    }
    if (!entry_ok) goto bad;
    fclose(file);
    return entry;
bad:
    fprintf(stderr, "%s: invalid or truncated load segment\n", path);
    exit(1);
}

static void load_initrd(const char *path)
{
    FILE *file = fopen(path, "rb");
    long size;
    if (!file || fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 ||
        size > 0x01000000L || fseek(file, 0, SEEK_SET) ||
        fread(ram + 0x04000000, 1, (size_t)size, file) != (size_t)size) {
        fprintf(stderr, "%s: invalid, unreadable, or larger than 16 MiB\n", path);
        exit(1);
    }
    fclose(file);
}

static void host_io(void)
{
    u8 bytes[UART_CAP];
    int i, n = read(0, bytes, sizeof bytes);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("stdin");
        exit(1);
    }
    for (i = 0; i < n; ++i) rv_input(bytes[i]);
    n = 0;
    while (rv_output_count()) bytes[n++] = rv_output();
    if (n && write(1, bytes, n) != (int)n) {
        perror("stdout");
        exit(1);
    }
}

int main(int argc, char **argv)
{
    u64 limit = 0, done = 0;
    int flags;

    if (argc < 3 || argc > 4) {
		fprintf(stderr, "usage: %s kernel.elf initrd.tar [max-instructions]\n", argv[0]);
        return 2;
    }
    if (argc == 4) limit = strtoull(argv[3], 0, 0);
    flags = fcntl(0, F_GETFL, 0);
    if (flags < 0 || fcntl(0, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("stdin nonblocking mode");
        return 1;
    }

    load_initrd(argv[2]);
    rv_init(load_elf(argv[1]));
    while (!limit || done < limit) {
        u32 batch = 200000;
        if (limit && limit - done < batch) batch = (u32)(limit - done);
        host_io();
        rv_run(batch, 100);
        host_io();
        done += batch;
    }
    return 0;
}
