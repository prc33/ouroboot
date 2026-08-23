# TCC fork, stripped to exactly two targets: i386 and riscv64. Nothing
# else (arm, arm64, riscv32, x86_64, c67, PE/Mach-O/COFF, bounds
# checking) is kept -- see docs/tcc-spike-findings.md and
# docs/riscv-port-findings.md for why these two specifically.
#
# One source tree, one target selected at build time -- this is how
# upstream TCC itself works (TCC_TARGET_* controls which *-gen.c/
# *-asm.c/*-link.c get compiled in), not a divergent fork per target.
#
#   make TARGET=i386
#   make TARGET=riscv64      # default
#
# `make selfcheck` is verified working end-to-end for TARGET=i386.
# For TARGET=riscv64 it currently HANGS at the first stage1 run --
# TCC self-hosted as a riscv64 binary, actually executed, is a known
# open issue (see docs/riscv-port-findings.md's final section). This
# is distinct from and does not affect `make` (TARGET=riscv64) itself:
# using this tcc as a HOST tool to compile riscv64 target code is
# fully proven -- it's what built and ran the entire musl and busybox
# test suites documented in docs/riscv-port-findings.md. Only running
# a *self-compiled* riscv64 tcc binary is unverified.
#
# riscv64 has NO integrated assembler for real instructions (see
# docs/riscv-port-findings.md for why, and why that turned out fine) --
# only directives (.weak/.set/.long/etc, used generically by both
# targets) plus a small set of compiler intrinsics
# (__builtin_riscv_syscall and friends) replace what inline asm and
# standalone .s files would otherwise need.

TARGET ?= riscv64

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wno-unused -fno-strict-aliasing -Wno-pointer-sign
DEFS    = -DTCC_TARGET_$(shell echo $(TARGET) | tr a-z A-Z) -DCONFIG_TCC_ASM \
          -DCONFIG_TRIPLET=\"$(TARGET)-linux-musl\"

.PHONY: all clean selfcheck stage1

all: tcc libtcc1.a

# ONE_SOURCE mode (tcc.h defaults it to 1): tcc.c textually includes
# libtcc.c, which includes tccpp.c/tccgen.c/tccelf.c/tccrun.c/tccasm.c
# and the target-specific *-gen.c/*-asm.c/*-link.c. One file, one
# compile -- see docs/tcc-spike-findings.md for why this replaced an
# earlier nine-file separate-compilation build that turned out to be
# silently linking only one of its own objects.
tcc: tcc.c tcc.h libtcc.c libtcc.h tccpp.c tccgen.c tccelf.c tccrun.c \
     tccasm.c tcctools.c \
     i386-gen.c i386-link.c i386-asm.c \
     riscv64-gen.c riscv64-link.c riscv64-asm.c
	$(CC) $(CFLAGS) $(DEFS) -o $@ tcc.c -lm -lpthread -ldl

# --- runtime helper archive ---
# i386: integer divide/shift helpers the ISA has no instruction for,
# long-double<->int conversion (lib/libtcc1.c, lib/alloca86*.S).
# riscv64: quad-precision (128-bit long double) soft-float helpers
# (lib/riscv64-softquad.c) plus the same libtcc1.c integer helpers,
# which needed portable C versions added (upstream only ships x86
# inline-asm ones) -- see docs/riscv-port-findings.md.
ifeq ($(TARGET),i386)
LIBTCC1_OBJS = lib/libtcc1.o lib/alloca86.o lib/alloca86-bt.o lib/dsohandle.o
else
LIBTCC1_OBJS = lib/libtcc1.o lib/riscv64-softquad.o lib/dsohandle.o
endif

libtcc1.a: tcc
	./tcc -B. -c lib/libtcc1.c -o lib/libtcc1.o -I.
ifeq ($(TARGET),i386)
	./tcc -B. -c lib/alloca86.S -o lib/alloca86.o -I.
	./tcc -B. -c lib/alloca86-bt.S -o lib/alloca86-bt.o -I.
else
	./tcc -B. -c lib/riscv64-softquad.c -o lib/riscv64-softquad.o -I.
endif
	./tcc -B. -c lib/dsohandle.c -o lib/dsohandle.o -I.
	./tcc -ar rcs $@ $(LIBTCC1_OBJS)

# --- stage1: tcc compiling ITSELF as a real target-arch program,
#     linked against musl (the libc our kernel will actually provide --
#     not host glibc). The genuine self-hosting proof: the output must
#     be run under an arch-appropriate environment (qemu-user here;
#     our own kernel later for i386; riscv64 kernel port not yet
#     started), not executed directly on this x86_64 host. ---
MUSL     ?= ../demo/musl-$(TARGET)
MUSLINC  = -I$(MUSL)/obj/include -I$(MUSL)/include -I$(MUSL)/arch/$(TARGET) -I$(MUSL)/arch/generic
MUSLLIBC = $(MUSL)/lib/libc.a
QEMU_USER = qemu-$(TARGET)-static

stage1: tcc libtcc1.a
	mkdir -p stage1
	./tcc -B. -I. -I./include $(MUSLINC) -nostdinc $(DEFS) \
		-c -o stage1/tcc.o tcc.c
	./tcc -B. -static -nostdinc -nostdlib -o stage1/tcc \
		$(MUSL)/lib/crt1.o $(MUSL)/lib/crti.o \
		stage1/tcc.o \
		$(MUSLLIBC) libtcc1.a $(MUSL)/lib/crtn.o

selfcheck: stage1
	@echo "--- stage0 (host ELF) vs stage1 ($(TARGET) ELF) ---"
	@file tcc stage1/tcc
	@echo "--- stage1 runs under $(QEMU_USER) and self-identifies: ---"
	$(QEMU_USER) ./stage1/tcc -v
	@echo "--- stage1 compiles a test program correctly: ---"
	echo 'int main(){return 42;}' > /tmp/selfcheck.c
	$(QEMU_USER) ./stage1/tcc -B. -I. -I./include $(MUSLINC) -nostdinc \
		-static -nostdlib -o /tmp/selfcheck \
		$(MUSL)/lib/crt1.o $(MUSL)/lib/crti.o /tmp/selfcheck.c \
		$(MUSLLIBC) libtcc1.a $(MUSL)/lib/crtn.o
	$(QEMU_USER) /tmp/selfcheck; test $$? -eq 42 && echo "OK: stage1 output runs and returns 42"

clean:
	rm -f tcc lib/*.o libtcc1.a
	rm -rf stage1
