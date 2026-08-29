# TCC fork with exactly three targets: i386, riscv64, and wasm32.
#
# One source tree, one target selected at build time -- this is how
# upstream TCC itself works (TCC_TARGET_* controls which *-gen.c/
# *-asm.c/*-link.c get compiled in), not a divergent fork per target.
#
#   make TARGET=i386
#   make TARGET=riscv64      # default
#
# `make selfcheck` is verified working end-to-end for BOTH targets --
# stage1 (a self-compiled target-arch tcc binary, run under qemu-user)
# compiles and correctly runs a test program for i386 and for riscv64.
# The riscv64 stage1 run used to hang indefinitely; that was a real
# codegen bug (gfunc_epilog mis-deriving sp from s0 for variadic
# functions -- corrupted the saved return address on every return from
# every varargs call), not a self-hosting-specific gap. Fixed; see
# docs/riscv-port-findings.md's closure section for the story.
#
# riscv64 includes an integrated RV64IMAFD assembler; musl's inline and
# standalone assembly is compiled directly.

TARGET ?= riscv64

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wno-unused -fno-strict-aliasing -Wno-pointer-sign
DEFS    = -DTCC_TARGET_$(shell echo $(TARGET) | tr a-z A-Z) \
          -DCONFIG_TRIPLET=\"$(TARGET)-linux-musl\"

ifeq ($(TARGET),i386)
BACKEND_SRCS = i386/i386-gen.c i386/i386-link.c i386/i386-asm.c \
               i386/i386-pair.c
RUNTIME_SRCS = i386/alloca86.S
else ifeq ($(TARGET),riscv64)
BACKEND_SRCS = risc/riscv64-gen.c risc/riscv64-link.c risc/riscv64-asm.c
RUNTIME_SRCS = risc/riscv64-softquad.c
else ifeq ($(TARGET),wasm32)
BACKEND_SRCS = wasm/wasm-gen.c wasm/wasm-link.c wasm/tccwasm.c
else
$(error unsupported TARGET '$(TARGET)')
endif

COMMON_SRCS = tcc.c utils.c tccpp.c tccgen.c expr.c symbols.c types.c vstack.c registers.c optimise.c tccelf.c tcctools.c
ifneq ($(TARGET),wasm32)
COMMON_SRCS += tccasm.c
endif
COMPILER_SRCS = $(COMMON_SRCS) $(BACKEND_SRCS)
BUILD_DIR = build/$(TARGET)
COMPILER_OBJS = $(addprefix $(BUILD_DIR)/,$(COMPILER_SRCS:.c=.o))
DEPFILES = $(COMPILER_OBJS:.o=.d)

# Compile each subsystem separately.  Target-specific directories prevent a
# target switch from reusing objects built with another ABI's definitions.

.PHONY: all clean selfcheck stage1 tcc wasm32-tcc libtcc1.a

ifeq ($(TARGET),wasm32)
all: wasm32-tcc
else
all: tcc libtcc1.a
endif

$(BUILD_DIR)/compiler: $(COMPILER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(COMPILER_OBJS) -lm -lpthread -ldl

tcc: $(BUILD_DIR)/compiler
	cp $< $@

wasm32-tcc: $(BUILD_DIR)/compiler
	cp $< $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEFS) -MMD -MP -c -o $@ $<

-include $(DEPFILES)

# --- runtime helper archive ---
# i386: integer divide/shift helpers the ISA has no instruction for,
# long-double<->int conversion (lib/libtcc1.c, i386/alloca86*.S).
# riscv64: quad-precision (128-bit long double) soft-float helpers
# (risc/riscv64-softquad.c) plus the same libtcc1.c integer helpers,
# which needed portable C versions added (upstream only ships x86
# inline-asm ones) -- see docs/riscv-port-findings.md.
ifeq ($(TARGET),i386)
LIBTCC1_OBJS = lib/libtcc1.o i386/alloca86.o lib/dsohandle.o
else
LIBTCC1_OBJS = lib/libtcc1.o risc/riscv64-softquad.o lib/dsohandle.o
endif

libtcc1.a: tcc lib/libtcc1.c lib/dsohandle.c $(RUNTIME_SRCS)
	./tcc -B. -c lib/libtcc1.c -o lib/libtcc1.o -I.
ifeq ($(TARGET),i386)
	./tcc -B. -c i386/alloca86.S -o i386/alloca86.o -I.
else
	./tcc -B. -c risc/riscv64-softquad.c -o risc/riscv64-softquad.o -I.
endif
	./tcc -B. -c lib/dsohandle.c -o lib/dsohandle.o -I.
	./tcc -ar rcs $@ $(LIBTCC1_OBJS)

# --- stage1: tcc compiling ITSELF as a real target-arch program,
#     linked against musl (the libc our kernel will actually provide --
#     not host glibc). The genuine self-hosting proof: the output must
#     be run under an arch-appropriate environment (qemu-user here;
#     our own kernel later for i386; riscv64 kernel port not yet
#     started), not executed directly on this x86_64 host. ---
MUSL     ?= ../musl-$(TARGET)
MUSLINC  = -I$(MUSL)/obj/include -I$(MUSL)/include -I$(MUSL)/arch/$(TARGET) -I$(MUSL)/arch/generic
MUSLLIBC = $(MUSL)/lib/libc.a
QEMU_USER = qemu-$(TARGET)-static

# Regression coverage for the riscv64-gen.c gfunc_epilog bug (fixed --
# see docs/riscv-port-findings.md's closure section): it corrupted the
# return address on every return from a variadic function. The
# selfcheck test below this one is 'int main(){return 42;}', which
# never calls one, so it couldn't have caught it.
define SELFCHECK_VA_SRC
#include <stdio.h>
int main(void) {
	char b[8];
	snprintf(b, sizeof(b), "%d", 42);
	return (b[0] == '4' && b[1] == '2' && b[2] == 0) ? 0 : 1;
}
endef
export SELFCHECK_VA_SRC

STAGE1_DIR = stage1/$(TARGET)
STAGE1_OBJS = $(addprefix $(STAGE1_DIR)/,$(COMPILER_SRCS:.c=.o))

$(STAGE1_DIR)/%.o: %.c tcc.h
	@mkdir -p $(dir $@)
	./tcc -B. -I. -I./include $(MUSLINC) -nostdinc $(DEFS) -c -o $@ $<

stage1: tcc libtcc1.a $(STAGE1_OBJS)
	./tcc -B. -static -nostdinc -nostdlib -o stage1/tcc \
		$(MUSL)/lib/crt1.o $(MUSL)/lib/crti.o \
		$(STAGE1_OBJS) \
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
	@echo "--- regression: stage1 correctly compiles+runs a variadic call ---"
	@echo "    (riscv64-gen.c's gfunc_epilog once corrupted the return"
	@echo "    address on every return from a varargs function -- the"
	@echo "    plain 'return 42' test above never calls one, so it never"
	@echo "    caught it. This does.)"
	echo "$$SELFCHECK_VA_SRC" > /tmp/selfcheck_va.c
	$(QEMU_USER) ./stage1/tcc -B. -I. -I./include $(MUSLINC) -nostdinc \
		-static -nostdlib -o /tmp/selfcheck_va \
		$(MUSL)/lib/crt1.o $(MUSL)/lib/crti.o /tmp/selfcheck_va.c \
		$(MUSLLIBC) libtcc1.a $(MUSL)/lib/crtn.o
	$(QEMU_USER) /tmp/selfcheck_va; test $$? -eq 0 && echo "OK: stage1-compiled variadic call ran correctly"
	@echo "--- regression: stage1 correctly loads/stores through a"
	@echo "    high absolute-address constant (bit 31 set) ---"
	@echo "    (riscv64-gen.c's load()/store() both used bare 'lui',"
	@echo "    which always sign-extends -- any constant address with"
	@echo "    bit 31 set (every physical address in QEMU riscv64"
	@echo "    virt's RAM, which starts at 0x80000000) came out as"
	@echo "    0xffffffff8....... instead of 0x000000008......., an"
	@echo "    out-of-range address (load() didn't even have a"
	@echo "    fallback -- it just errored out). Found via the kernel"
	@echo "    port, which is the first code in this project to use a"
	@echo "    constant in that range as a pointer -- ordinary"
	@echo "    userspace code on a normal 64-bit Linux essentially"
	@echo "    never does; mmap'd here at a fixed address purely so"
	@echo "    this test can safely dereference one.)"
	printf '#include <sys/mman.h>\n#include <stdint.h>\nint main(void){\n\tvoid *p = mmap((void*)0x80305000UL, 4096, PROT_READ|PROT_WRITE, MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);\n\tif (p == MAP_FAILED) return 2;\n\t*(volatile uint32_t*)0x80305000UL = 0x12345678;\n\tuint32_t v = *(volatile uint32_t*)0x80305000UL;\n\treturn v == 0x12345678 ? 0 : 1;\n}\n' > /tmp/selfcheck_hiaddr.c
	$(QEMU_USER) ./stage1/tcc -B. -I. -I./include $(MUSLINC) -nostdinc \
		-static -nostdlib -o /tmp/selfcheck_hiaddr \
		$(MUSL)/lib/crt1.o $(MUSL)/lib/crti.o /tmp/selfcheck_hiaddr.c \
		$(MUSLLIBC) libtcc1.a $(MUSL)/lib/crtn.o
	$(QEMU_USER) /tmp/selfcheck_hiaddr; test $$? -eq 0 && echo "OK: stage1-compiled high-address load/store ran correctly"
clean:
	rm -f tcc wasm32-tcc lib/*.o i386/*.o risc/*.o libtcc1.a
	rm -rf build stage1
