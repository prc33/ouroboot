#ifndef TCC_ELF_H
#define TCC_ELF_H

#include "common.h"
#include "target.h"
#include "elf.h"
#include "utils.h"

#if PTR_SIZE == 8
# define ELFCLASSW ELFCLASS64
# define ElfW(type) Elf##64##_##type
# define ELFW(type) ELF##64_##type
#else
# define ELFCLASSW ELFCLASS32
# define ElfW(type) Elf##32##_##type
# define ELFW(type) ELF##32_##type
#endif

#define ARMAG "!<arch>\012"
#define ARFMAG "`\n"

typedef struct ArchiveHeader {
    char ar_name[16];
    char ar_date[12];
    char ar_uid[6];
    char ar_gid[6];
    char ar_mode[8];
    char ar_size[10];
    char ar_fmag[2];
} ArchiveHeader;

ST_FUNC int tcc_tool_ar(int argc, char **argv);

#endif
