/* Native command-line front end for the RV64 emulator. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rv64.c"

static u16 elf16(const u8 *p)
{
    return (u16)p[0] | (u16)p[1] << 8;
}

static u32 elf32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u64 elf64(const u8 *p)
{
    return elf32(p) | (u64)elf32(p + 4) << 32;
}

static u64 load_elf(const char *path)
{
    FILE *file = fopen(path, "rb");
    u8 header[64], program[56];
    u64 phoff, offset, address, filesz, memsz;
    u32 i, phnum, phsize;

    if (!file || fread(header, 1, sizeof header, file) != sizeof header ||
        elf32(header) != 0x464c457f || header[4] != 2 || header[5] != 1 ||
        elf16(header + 18) != 243) {
        fprintf(stderr, "%s: expected a 64-bit little-endian RISC-V ELF file\n", path);
        exit(1);
    }
    phoff = elf64(header + 32);
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
    }
    fclose(file);
    return elf64(header + 24);
bad:
    fprintf(stderr, "%s: invalid or truncated load segment\n", path);
    exit(1);
}

static void host_input(void)
{
    u8 bytes[256];
    int i, n = read(0, bytes, sizeof bytes);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("stdin");
        exit(1);
    }
    for (i = 0; i < n; ++i) rv_input(bytes[i]);
}

static void host_output(void)
{
    u8 bytes[UART_CAP];
    u32 n = 0;
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

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s kernel.elf [max-instructions]\n", argv[0]);
        return 2;
    }
    if (argc == 3) limit = strtoull(argv[2], 0, 0);
    flags = fcntl(0, F_GETFL, 0);
    if (flags < 0 || fcntl(0, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("stdin nonblocking mode");
        return 1;
    }

    rv_init(load_elf(argv[1]));
    while (!limit || done < limit) {
        u32 batch = 200000;
        if (limit && limit - done < batch) batch = (u32)(limit - done);
        host_input();
        rv_run(batch, 100);
        host_output();
        done += batch;
    }
    return 0;
}
