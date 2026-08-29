#ifndef TCC_COMMON_H
#define TCC_COMMON_H

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "config.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <setjmp.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#ifndef CONFIG_TCC_STATIC
# include <dlfcn.h>
#endif

#ifndef O_BINARY
# define O_BINARY 0
#endif
#ifndef offsetof
# define offsetof(type, field) ((size_t) &((type *)0)->field)
#endif
#ifndef countof
# define countof(tab) (sizeof(tab) / sizeof((tab)[0]))
#endif

#define NORETURN __attribute__((noreturn))
#define ALIGNED(x) __attribute__((aligned(x)))
#define PRINTF_LIKE(x,y) __attribute__((format(printf, (x), (y))))
#define IS_DIRSEP(c) ((c) == '/')
#define ST_INLN
#define ST_FUNC
#define ST_DATA extern
#ifndef PUB_FUNC
# define PUB_FUNC
#endif

#ifdef __TINYC__
# undef __attribute__
#endif

#endif
