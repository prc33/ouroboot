# Kernel build. Everything here is built with OUR OWN stripped TCC --
# not host gcc -- per the plan's hard constraint that kernel/ and
# emulator/ must self-host. See ../compiler/.
#
#   make               # ARCH=i386 (default)
#   make ARCH=riscv64
#
# NOTE: `clean` (like `all`/`test`) only removes the OBJS for whatever
# ARCH it's invoked with -- `make clean` alone leaves riscv64's .o
# files behind, `make ARCH=riscv64 clean` leaves i386's. If you switch
# ARCH and something acts stale (e.g. a compiler fix doesn't seem to
# apply -- confirmed the hard way while verifying this port), that's
# almost always a leftover .o from the other ARCH's build, not
# rebuilt because its .c/.S source didn't change even though
# ../compiler/tcc did. Always `make ARCH=<target> clean` before
# switching. Same gotcha the compiler's own Makefile documents for
# `TARGET=`.
#
# Every existing i386 file/behavior is unchanged by the riscv64 port --
# see docs/riscv-port-findings.md's kernel-port section. The only
# compiler this Makefile ever invokes, for either ARCH, is
# ../compiler/tcc -- no rule here calls riscv64-linux-gnu-as or
# anything outside this project's own toolchain.
#
# docs/kernel-arch-split-plan.md: every genuinely architecture-specific
# file lives under arch/i386/ or arch/risc/ (mirroring
# ../compiler/i386/, ../compiler/risc/); everything else (mm/, sched/,
# drivers/kprintf.c, kernel.h) is real architecture-neutral C, shared
# by both ARCHs, not two copies that happen to agree.

ARCH ?= i386

TCC     := ../compiler/tcc
TCCDIR  := ../compiler
TCCINC  := $(TCCDIR)/include

RISCV64_USER_PROGRAMS := hello proc_test proc_fork_test proc_exec_test \
	init_test interactive_test exec_target fetch
RISCV64_USER_ELFS := $(addprefix user_test/,$(addsuffix _riscv64.elf,$(RISCV64_USER_PROGRAMS)))
MUSL_DIR := ../musl-riscv64

CFLAGS  := -B$(TCCDIR) -I$(TCCINC) -I. -Wall -nostdinc -fno-stack-protector -ffreestanding

ifeq ($(ARCH),i386)
LDFLAGS := -B$(TCCDIR) -nostdlib -static -Wl,-Ttext=0x100000
# checkpoint 20 (docs/repo-review-2026-08-26.md section 4): arch/i386/pit.o
# (the PIT-driven timer, only ever used to pace the superseded P4
# fixed-2-task scheduler demo), arch/i386/usermode.o (enter_usermode(),
# only ever used to enter ring3 directly, outside the process table --
# superseded by arch/i386/process.c's i386_trap_return()), and
# arch/i386/task.o (that same P4 scheduler's own struct task/task_yield
# API, superseded by sched/process.c from checkpoint 17 onward) are
# deliberately *not* here -- the product kernel never calls any of
# them. All three are added back for I386_CHECKPOINT_OBJS below, which
# the historical P4/P5 regression chain (test/i386_checkpoints.c) still
# exercises. arch/i386/switch_context.o stays: it's the generic
# coroutine primitive both the old P4 scheduler *and* the real process
# table (sched/process.c, via arch/i386/process.c) build on.
OBJS := arch/i386/boot.o arch/i386/kmain.o arch/i386/serial.o drivers/kprintf.o \
        arch/i386/gdt.o arch/i386/idt.o arch/i386/isr_stubs.o arch/i386/pic.o \
        arch/i386/syscall_stub.o arch/i386/syscall.o syscall_common.o syscall_posix.o \
        mm/pmm.o arch/i386/paging.o mm/paging_common.o mm/elf.o mm/ramfs.o mm/tar.o \
        arch/i386/switch_context.o \
        arch/i386/process.o arch/i386/i386_process_return.o sched/process.o \
        arch/i386/kend.o
else ifeq ($(ARCH),riscv64)
CFLAGS  += -DKERNEL_ARCH_RISCV64
# Confirmed (readelf -l on the output): TCC's own linker places the
# LOAD segment's vaddr exactly at -Ttext's address, entry point
# included, with no separate header-page reservation below it the way
# GNU ld does without -N -- so unlike the standalone validation in
# docs/riscv-port-findings.md (which used riscv64-linux-gnu-ld), no
# extra flag is needed here. Still worth knowing: QEMU's riscv virt
# machine jumps to the lowest PT_LOAD vaddr, not e_entry -- they just
# happen to always agree with TCC's linker.
LDFLAGS := -B$(TCCDIR) -nostdlib -static -Wl,-Ttext=0x80200000
# arch/risc/riscv64_boot.o supplies the entry point and must be first;
# arch/risc/riscv64_kend.o must be last, like arch/i386/kend.o.
# arch/risc/riscv64_task.o (the P4 fixed-2-task scheduler, superseded
# by sched/process.o from checkpoint 6 onward) is deliberately *not*
# here -- the product kernel never calls it. It's added back for
# RISCV64_CHECKPOINT_OBJS below, which the historical P1-P10 regression
# chain (kernel/test/riscv64_checkpoints.c) still exercises. See
# docs/kernel-complexity-review.md section 1.
#
# sched/process.o (the general process table -- generic, see its own
# file comment) and arch/risc/riscv64_process.o (the small
# riscv64-specific half it calls through: struct regs, the trap-return
# mechanism, the hand-built initial-kernel-stack-frame convention) used
# to be one file -- see docs/kernel-arch-split-plan.md.
OBJS := arch/risc/riscv64_boot.o arch/risc/riscv64_entry.o arch/risc/riscv64_kmain.o \
        arch/risc/riscv64_serial.o drivers/kprintf.o \
        arch/risc/riscv64_trap_entry.o arch/risc/riscv64_trap.o arch/risc/riscv64_timer.o \
        arch/risc/riscv64_usermode.o arch/risc/riscv64_syscall.o arch/risc/riscv64_process.o \
        syscall_common.o syscall_posix.o \
        mm/pmm.o arch/risc/riscv64_paging.o mm/paging_common.o mm/elf.o mm/ramfs.o mm/tar.o \
        arch/risc/riscv64_switch_context.o \
        sched/process.o \
        arch/risc/riscv64_kend.o
else
$(error unknown ARCH '$(ARCH)' -- expected i386 or riscv64)
endif

.PHONY: all test test-wasm shell-wasm test-initrd test-initrd-wasm \
	test-selfhost tcc-initrd selfhost-initrd selfhost-initrd-i386 \
	test-busybox clean

all: kernel.elf

# One pattern rule for every .S file, wherever it lives -- the eleven
# individual rules this used to be (one per file, each an identical
# recipe) were never about the *command* differing, only about make
# needing each target named; a pattern rule already covers subdirectory
# targets the same way %.o: %.c below always has (see
# docs/kernel-arch-split-plan.md).
%.o: %.S
	$(TCC) $(CFLAGS) -c -o $@ $<

%.o: %.c kernel.h
	$(TCC) $(CFLAGS) -c -o $@ $<

kernel.elf: $(OBJS)
	$(TCC) $(LDFLAGS) -o $@ $(OBJS)

# Ordinary initrd executables, built by the same compiler and musl used by the
# rest of the project. They are files, not generated C source in kernel.elf.
$(RISCV64_USER_ELFS): user_test/%_riscv64.elf: user_test/%_riscv64.c
	$(TCC) -B$(TCCDIR) -I$(TCCINC) -I$(MUSL_DIR)/obj/include \
		-I$(MUSL_DIR)/include -I$(MUSL_DIR)/arch/riscv64 \
		-I$(MUSL_DIR)/arch/generic -nostdinc -static -nostdlib -o $@ \
		$(MUSL_DIR)/lib/crt1.o $(MUSL_DIR)/lib/crti.o $< \
		$(MUSL_DIR)/lib/libc.a $(TCCDIR)/libtcc1.a $(MUSL_DIR)/lib/crtn.o

# BusyBox is built by demo/build-busybox-riscv64.sh. Keep the generated
# program out of Git, but make the initrd's dependency on that build output
# explicit so a clean checkout needs no manual copy step.
user_test/busybox_riscv64.elf: ../busybox-riscv64/busybox_unstripped
	cp $< $@

user_test/user_test_riscv64.elf: user_test/user_test_riscv64.c
	$(TCC) -B$(TCCDIR) -nostdinc -nostdlib -static -Wl,-Ttext=0x800000 -o $@ $<

# i386 counterpart of the riscv64 rule above -- same shape, different
# musl tree (checkpoint 19, docs/kernel-arch-split-plan.md). $(TCC)
# itself must already be built TARGET=i386 for this to link correctly
# (same "rebuild the compiler for the ARCH you're about to build"
# discipline the rest of this Makefile's own comments describe).
MUSL_DIR_I386 := ../musl-i386
user_test/%_i386.elf: user_test/%_i386.c
	$(TCC) -B$(TCCDIR) -I$(TCCINC) -I$(MUSL_DIR_I386)/obj/include \
		-I$(MUSL_DIR_I386)/include -I$(MUSL_DIR_I386)/arch/i386 \
		-I$(MUSL_DIR_I386)/arch/generic -nostdinc -static -nostdlib -o $@ \
		$(MUSL_DIR_I386)/lib/crt1.o $(MUSL_DIR_I386)/lib/crti.o $< \
		$(MUSL_DIR_I386)/lib/libc.a $(TCCDIR)/libtcc1.a $(MUSL_DIR_I386)/lib/crtn.o

ifeq ($(ARCH),i386)
# kernel.elf itself is the product boot (arch/i386/kmain.c's kmain(),
# built with the default OBJS above) -- `test` needs the separate
# historical-checkpoint-chain variant below instead, mirroring
# riscv64's own RISCV64_CHECKPOINT_OBJS exactly (checkpoint 20,
# docs/repo-review-2026-08-26.md section 4): arch/i386/task.o/pit.o/
# usermode.o are added back here since only the checkpoint chain
# (test/i386_checkpoints.c) still uses them, and
# arch/i386/kmain_checkpoints.o (this same kmain.c, compiled with
# -DKERNEL_CHECKPOINTS so its own kmain() calls run_checkpoint_boot()
# instead of booting straight to BusyBox) replaces the product
# arch/i386/kmain.o.
I386_CHECKPOINT_OBJS := $(patsubst arch/i386/kmain.o,arch/i386/kmain_checkpoints.o,$(OBJS)) \
	arch/i386/task.o arch/i386/pit.o arch/i386/usermode.o test/i386_checkpoints.o

arch/i386/kmain_checkpoints.o: arch/i386/kmain.c kernel.h
	$(TCC) $(CFLAGS) -DKERNEL_CHECKPOINTS -c -o $@ $<

kernel-checkpoints.elf: $(I386_CHECKPOINT_OBJS)
	$(TCC) $(LDFLAGS) -o $@ $(I386_CHECKPOINT_OBJS)

# P5 checkpoint 2's real musl+TCC binary (user_test/hello_i386.c,
# checkpoint 20) is now a real initrd file instead of an embedded hex
# dump -- see test/i386_checkpoints.c's own header comment. This is
# the checkpoint chain's *own* small initrd, distinct from
# test-initrd's/test-busybox's product-boot ones below.
test: kernel-checkpoints.elf user_test/hello_i386.elf
	rm -rf /tmp/initrd-i386-cp-stage && mkdir -p /tmp/initrd-i386-cp-stage
	cp user_test/hello_i386.elf /tmp/initrd-i386-cp-stage/hello
	tar cf initrd-i386-checkpoints.tar -C /tmp/initrd-i386-cp-stage hello
	python3 test/boot_test.py kernel-checkpoints.elf \
		--qemu-arg=-initrd --qemu-arg=initrd-i386-checkpoints.tar \
		--must-contain "multiboot magic OK" \
		--must-contain "mem_lower" \
		--must-contain "gdt: loaded" \
		--must-contain "idt: loaded" \
		--must-contain "pic: remapped" \
		--must-contain "breakpoint: int3 handled and resumed OK" \
		--must-contain "pmm:" \
		--must-contain "paging: identity-mapped" \
		--must-contain "initrd: 1 file(s) loaded from tar" \
		--must-contain "COW test: parent=1 child=2 OK" \
		--must-contain "pit: channel 0" \
		--must-contain "TASK A" \
		--must-contain "TASK B" \
		--must-contain "scheduler: 6 switches across 2 tasks OK" \
		--must-contain "P4 checkpoint 2 OK" \
		--must-contain "syscall: dispatch installed" \
		--must-contain "ring3: entering userspace" \
		--must-contain "hello from ring3 via int 0x80" \
		--must-contain "process exited with code 0" \
		--must-contain "ring3 test OK" \
		--must-contain "P5 checkpoint 1 OK" \
		--must-contain "elf: loaded, entry=" \
		--must-contain "elf: entering userspace" \
		--must-contain "syscall: dispatch installed" \
		--must-contain "hello from musl+tcc, sum=285" \
		--must-contain "P5 checkpoint 2 OK" \
		--must-not-contain "FATAL" \
		--must-not-contain "UNHANDLED EXCEPTION" \
		--must-not-contain "PAGE FAULT"

# checkpoint 19: real process table + real fork()/wait4()/COW, driven
# by a genuine Multiboot module -- QEMU's own `-initrd` flag, unlike
# riscv64's `-device loader,addr=...` (i386 has no fixed physical
# initrd address the way arch/risc/riscv64_memmap.h's RV64_INITRD_BASE
# is; a Multiboot module's address is whatever the loader picks,
# discovered at runtime via kmain.c's own multiboot_info parsing).
# Packaged under the name "busybox" -- deliberately, even though this
# specific target still uses the smaller, deterministic fork/wait4/COW
# test payload rather than a real BusyBox (test-busybox below is that
# one): it's the exact same filename the product boot path
# (kmain()'s own #else branch, arch/i386/kmain.c) actually loads, so
# this target keeps exercising process_create_from_elf_argv()'s own real
# code path, not a parallel one.
test-initrd: kernel.elf user_test/proc_fork_test_i386.elf
	rm -rf /tmp/initrd-i386-stage && mkdir -p /tmp/initrd-i386-stage
	cp user_test/proc_fork_test_i386.elf /tmp/initrd-i386-stage/busybox
	tar cf initrd-i386.tar -C /tmp/initrd-i386-stage busybox
	python3 test/boot_test.py kernel.elf \
		--qemu-arg=-initrd --qemu-arg=initrd-i386.tar \
		--must-contain "multiboot: initrd module at" \
		--must-contain "initrd: 1 file(s) loaded from tar" \
		--must-contain "process: init process created" \
		--must-contain "child counter=42" \
		--must-contain "parent counter=1 reaped_pid_ok=1 child_status=7" \
		--must-contain "process: all processes exited" \
		--must-not-contain "FATAL" \
		--must-not-contain "UNHANDLED EXCEPTION" \
		--must-not-contain "PAGE FAULT"

# checkpoint 19: a real upstream BusyBox (demo/build-busybox-i386.sh --
# see docs/busybox-findings.md for how it's built and made to link
# under our own stripped TCC), interactively driving real ash, real
# fork()/exec() (`expr` is BusyBox's own separate applet dispatch, not
# a shell builtin), and the writable ramfs (`echo ... > file`) -- the
# fullest exercise of this whole i386 self-hosting phase's syscall
# surface short of actually self-hosting TCC itself (that's
# test-selfhost's own job, riscv64-only so far).
#
# user_test/busybox_i386.elf isn't built by this Makefile (unlike the RISC-V
# copy rule above, which takes demo/build-busybox-riscv64.sh's output). It is
# a real ~500KB+ binary, deliberately not committed: run
# `demo/build-busybox-i386.sh` (needs `../compiler/tcc` already built
# TARGET=i386 and ../musl-i386 already built) and copy the resulting
# demo/busybox/busybox_unstripped here first.
test-busybox: kernel.elf user_test/busybox_i386.elf
	rm -rf /tmp/initrd-i386-bb-stage && mkdir -p /tmp/initrd-i386-bb-stage
	cp user_test/busybox_i386.elf /tmp/initrd-i386-bb-stage/busybox
	tar cf initrd-i386-bb.tar -C /tmp/initrd-i386-bb-stage busybox
	python3 test/boot_test.py kernel.elf \
		--qemu-arg=-initrd --qemu-arg=initrd-i386-bb.tar \
		--stdin-input "\r\recho hello ash\rexpr 6 \\* 7\recho writable-fs-works > wtest\rcat wtest\rls\rexit 7\r" \
		--must-contain "BusyBox v1.36.1" \
		--must-contain "hello ash" \
		--must-contain "42" \
		--must-contain "writable-fs-works" \
		--must-contain "wtest" \
		--must-contain "process: all processes exited" \
		--must-not-contain "FATAL" \
		--must-not-contain "UNHANDLED EXCEPTION" \
		--must-not-contain "PAGE FAULT"

# checkpoint 19's closure: the same "TCC compiles TCC, inside our
# kernel" proof riscv64's own test-selfhost has had since checkpoint
# 14, now for i386 -- see that target's own comment (below, in the
# riscv64 branch) for what each piece is and why. Mirrors it file for
# file: test/build-selfhost-initrd-i386.sh in place of
# test/build-selfhost-initrd.sh, compiler/stage1/tcc built TARGET=i386
# instead of TARGET=riscv64, musl-i386 instead of musl-riscv64.
selfhost-initrd-i386: user_test/busybox_i386.elf
	$(MAKE) -C ../compiler clean
	$(MAKE) -C ../compiler TARGET=i386 stage1
	sh test/build-selfhost-initrd-i386.sh .. $@.tar

I386_SELFHOST_INPUT := \r\rls\rls tcc-src\rcd tcc-src\rpwd\rcd /\rash /selfhost.sh\rexit 7\r

test-selfhost: kernel.elf
	$(MAKE) selfhost-initrd-i386
	python3 test/boot_test.py kernel.elf \
		--qemu-arg=-initrd --qemu-arg=selfhost-initrd-i386.tar \
		--mem 128 --timeout 60 \
		--stdin-input "$(I386_SELFHOST_INPUT)" \
		--must-contain "SELFHOST: stage1 compiling TCC" \
		--must-contain "tcc-src" \
		--must-contain "musl" \
		--must-contain "/tcc-src" \
		--must-contain "SELFHOST: stage2 runs" \
		--must-contain "tcc version 0.9.27-rv64min (i386 Linux)" \
		--must-contain "hello from stage2 tcc" \
		--must-contain "SELFHOST: complete" \
		--must-contain "process: all processes exited" \
		--must-not-contain "FATAL" \
		--must-not-contain "PAGE FAULT"
else
# Shared between `test` (QEMU) and `test-wasm` (our C/Wasm emulator --
# see docs/emulator-plan.md's P2) so the two never silently drift
# apart -- both are asserting the exact same checkpoints, just under
# two different implementations of the same ISA.
RISCV64_TEST_ASSERTIONS := \
	--must-contain "boot: entered kmain in S-mode" \
	--must-contain "trap: stvec installed" \
	--must-contain "breakpoint: ebreak handled and resumed OK" \
	--must-contain "pmm:" \
	--must-contain "pmm: reserve test OK" \
	--must-contain "paging: Sv39 identity-mapped" \
	--must-contain "COW test: parent=1 child=2 OK" \
	--must-contain "COW test: PTE_USER preserved after copy OK" \
	--must-contain "timer: Sstc armed" \
	--must-contain "TASK A" \
	--must-contain "TASK B" \
	--must-contain "scheduler: 6 switches across 2 tasks OK" \
	--must-contain "syscall: dispatch installed" \
	--must-contain "ring3: entering userspace" \
	--must-contain "hello from ring3 via ecall" \
	--must-contain "process exited with code 0" \
	--must-contain "ring3 test OK" \
	--must-contain "P5 checkpoint 1 OK" \
	--must-contain "elf: loaded, entry=" \
	--must-contain "elf: entering userspace" \
	--must-contain "hello from musl+tcc, sum=285" \
	--must-contain "P5 checkpoint 2 OK" \
	--must-contain "A1 B1 A2 B2 A3 B3" \
	--must-contain "P6 checkpoint OK" \
	--must-contain "child counter=42" \
	--must-contain "parent counter=1 reaped_pid_ok=1 child_status=7" \
	--must-contain "P7 checkpoint OK" \
	--must-contain "argv1=execved_ok" \
	--must-contain "hello from ramfs" \
	--must-contain "exec test: child_status=42" \
	--must-contain "P8 checkpoint OK" \
	--must-contain "hello from ash" \
	--must-contain "builtin ok" \
	--must-contain "init test: ash_status=5" \
	--must-contain "P9 checkpoint OK" \
	--must-contain "built-in shell (ash)" \
	--must-contain "hello ash" \
	--must-contain "greeting" \
	--must-contain "exec_target" \
	--must-contain "wtest" \
	--must-contain "process: all processes exited" \
	--must-contain "P10 checkpoint OK" \
	--must-not-contain "FATAL" \
	--must-not-contain "UNHANDLED EXCEPTION" \
	--must-not-contain "UNHANDLED INTERRUPT" \
	--must-not-contain "PAGE FAULT"

# checkpoint 10: real commands fed to real interactive ash via real
# stdin (kernel/test/riscv64_checkpoints.c's run_interactive_test) -- exactly
# what a person typing at the browser terminal would produce, just
# delivered all at once instead of paced by real keystrokes (see
# test/boot_test.py's and emulator/test/boot.mjs's own comments on their
# respective --stdin-input/--input flags for why that's still a
# faithful correctness check). `exit 7`'s specific code isn't asserted
# here (no wait4() catches it -- run_interactive_test doesn't fork,
# ash *is* the whole process, same as a real login shell) -- the real
# proof is closer to home: if ash's interactive read loop didn't
# correctly terminate on `exit`, the process tree would never drain
# and "P10 checkpoint OK" would simply never print.
#
# Literal two-character "\n"/"\r" here, not real newline/CR bytes --
# both test/boot_test.py and emulator/test/boot.mjs
# --input decode those escapes themselves, deliberately sidestepping
# the make-variable/shell-recipe/subprocess-argv quoting stack a real
# embedded byte would have to survive unmangled through all three.
#
# \r, not \n, terminates each line here -- deliberately, and this is
# the real regression test for a real bug: a real terminal always
# sends '\r' (0x0D) for Enter, never '\n' (confirmed by instrumenting
# xterm.js's term.onData in the browser demo -- see arch/risc/riscv64_syscall.c's
# sys_read for the ICRNL-by-hand translation this exercises). Using
# \n here instead would silently stop testing the exact byte a real
# human's Enter key -- or a real terminal attached to a real QEMU
# `-serial stdio` in interactive mode -- actually produces.
#
# Leading \r\r: real QEMU's `-serial stdio` loses exactly the first 2
# bytes fed in over a pipe, consistently -- confirmed real (traced
# down to arch/risc/riscv64_syscall.c's sys_read never even seeing them)
# and confirmed harmless for real interactive use (a human can't type
# before this kernel's own boot sequence, which is what's actually
# consuming those 2 byte-times, finishes -- see arch/risc/riscv64_serial.c's
# serial_init() and this repo's own commit history for the full
# mechanism). Two throwaway blank lines absorb it losslessly: ash
# treats an empty line as a no-op, and losing exactly 2 of the 2 bytes
# that make them up leaves the real commands after them untouched
# either way. A no-op for the Wasm emulator too (it loses no bytes), just two
# harmless empty prompts.
#
# `ls` in the middle: the real regression test for a real bug (a user
# report, not something this test suite happened to catch on its own)
# -- open(".") failed with ENOENT (mm/ramfs.c has no real directory
# entries to match against) even though stat(".") already worked, so
# every `ls` in the browser demo failed with "can't open '.'". Fixed
# by arch/risc/riscv64_syscall.c's sys_openat/sys_getdents64 (see their own
# comments); "greeting"/"exec_target" below are mm/ramfs.c's own real
# files, only reachable in the transcript through a real, successful
# directory listing, not (unlike "busybox") also produced some other
# way (the boot banner prints "BusyBox", capital B, a different
# string).
#
# `echo writable-fs-works > wtest` then a second `ls`: the writable-
# ramfs regression test (checkpoint 12) -- real shell I/O redirection
# (open(O_CREAT|O_TRUNC|O_WRONLY), dup3() onto fd 1, write()), not
# faked. "wtest" below is checked the same rigorous way as "greeting"/
# "exec_target" above: it can only appear in this transcript via the
# *second* ls's own real getdents64 listing (a directory entry it
# creates at runtime, not embedded anywhere) -- the typed command line
# itself, echoed back character-by-character, would put "wtest" right
# after a literal ">" with no line break, a different, distinguishable
# context grep/--must-contain doesn't need to tell apart here because
# only the bare-word ls listing occurrence proves the file (and thus
# the write) is real.
RISCV64_INTERACTIVE_INPUT := \r\recho hello ash\rls\recho writable-fs-works > wtest\rls\rexit 7\r

# --mem 128: a real, pre-existing bug this checkpoint's own initrd
# work found (not something it caused) -- test/boot_test.py's own
# DEFAULT_MEM_MB is 32, and nothing here ever overrode it, so every
# `make ARCH=riscv64 test` this project has ever run gave QEMU only
# 32MB of *real* RAM despite arch/risc/riscv64_memmap.h's RV64_MEM_TOP
# hardcoding a 128MB assumption everywhere else (pmm_init(),
# paging_init(), ...) -- a mismatch nothing ever actually hit before
# (every checkpoint's real memory usage happened to stay comfortably
# under 32MB) until RV64_INITRD_BASE (64MB in) tried to read real
# memory that, under the 32MB default, genuinely wasn't backed by
# anything -- confirmed as a real (not virtual/page-table) fault:
# scause 5 "Load access fault", not 13 "Load page fault", at exactly
# 0x84000000. emulator/js/memory.js's own RAM_SIZE was *already*
# hardcoded to the full 128MB, which is why `test-js`/`shell-js` never
# had this problem -- only the real-QEMU path was under-provisioned.
# Explicit here now so QEMU's real configuration actually matches what
# the kernel has always assumed it has.
# kernel.elf itself is the product boot (arch/risc/riscv64_kmain.c's
# kmain(), built with the default OBJS above) -- `test`/`test-wasm`
# need the separate historical-checkpoint-chain variant below instead,
# since that's what still exercises the P1-P10 sequence
# (kernel/test/riscv64_checkpoints.c). See
# docs/kernel-complexity-review.md section 1: this used to be inverted
# (the checkpoint chain was the *default* kmain(), and a
# KERNEL_DIRECT_SHELL flag opted into the product boot instead) -- the
# product boot is unconditional now, so it's this variant that needs
# the flag.
RISCV64_CHECKPOINT_OBJS := $(patsubst arch/risc/riscv64_kmain.o,arch/risc/riscv64_kmain_checkpoints.o,$(OBJS)) \
	arch/risc/riscv64_task.o test/riscv64_checkpoints.o

arch/risc/riscv64_kmain_checkpoints.o: arch/risc/riscv64_kmain.c kernel.h
	$(TCC) $(CFLAGS) -DKERNEL_CHECKPOINTS -c -o $@ $<

kernel-checkpoints.elf: $(RISCV64_CHECKPOINT_OBJS)
	$(TCC) $(LDFLAGS) -o $@ $(RISCV64_CHECKPOINT_OBJS)

test: kernel-checkpoints.elf initrd.tar
	python3 test/boot_test.py kernel-checkpoints.elf \
		--qemu-binary qemu-system-riscv64 \
		--qemu-arg=-M --qemu-arg=virt \
		--qemu-arg=-bios --qemu-arg=default \
		--qemu-arg=-cpu --qemu-arg=rv64,sstc=true \
		--qemu-arg=-device --qemu-arg=loader,file=initrd.tar,addr=0x84000000 \
		--mem 128 \
		--stdin-input "$(RISCV64_INTERACTIVE_INPUT)" \
		$(RISCV64_TEST_ASSERTIONS)

# TCC-built C/Wasm emulator instead of QEMU. Reaching P10 implies that all
# earlier boot, process, filesystem, and interactive-shell work completed.
test-wasm: kernel-checkpoints.elf initrd.tar
	$(MAKE) -C ../emulator web/rv64.wasm
	node ../emulator/test/boot.mjs kernel-checkpoints.elf 340000000 \
		"$(RISCV64_INTERACTIVE_INPUT)" 'P10 checkpoint OK' initrd.tar

# Product boot, used for interactive development and the emulator's
# startup-time budget: kernel.elf itself now, since it *is* the direct
# path straight to an interactive shell -- no separate flag/variant
# needed any more. Covers both properties this target exists for: real
# interactive use (RISCV64_INTERACTIVE_INPUT's `echo`/`ls`/writable-
# file/`exit` round trip actually runs and produces "hello ash" + a
# real ls listing + a real created-and-read-back file + a clean
# process-table drain, not just a prompt appearing -- reaching
# "built-in shell (ash)" alone doesn't prove typed input is ever read).
shell-wasm: kernel.elf initrd.tar
	$(MAKE) -C ../emulator test-shell

# The ordinary explicit filesystem image: BusyBox, the process-test fixtures,
# and one plain data file. The host tar packages it; QEMU or the Wasm host puts
# it at RV64_INITRD_BASE. The kernel contains no file payload bytes.
initrd.tar: test/build-initrd.sh test/initrd-fixture/from-initrd.txt \
		test/initrd-fixture/greeting test/initrd-fixture/test.sh \
		user_test/busybox_riscv64.elf user_test/user_test_riscv64.elf \
		$(RISCV64_USER_ELFS)
	sh test/build-initrd.sh .. $@

test-initrd: kernel.elf initrd.tar
	python3 test/boot_test.py kernel.elf \
		--qemu-binary qemu-system-riscv64 \
		--qemu-arg=-M --qemu-arg=virt \
		--qemu-arg=-bios --qemu-arg=default \
		--qemu-arg=-cpu --qemu-arg=rv64,sstc=true \
		--qemu-arg=-device --qemu-arg=loader,file=initrd.tar,addr=0x84000000 \
		--mem 128 \
		--stdin-input "\r\rcat from-initrd.txt\rexit 7\r" \
		--must-contain "initrd: 12 file(s) loaded" \
		--must-contain "hello from a real initrd tar" \
		--must-contain "process: all processes exited" \
		--must-not-contain "FATAL" \
		--must-not-contain "UNHANDLED EXCEPTION" \
		--must-not-contain "UNHANDLED INTERRUPT" \
		--must-not-contain "PAGE FAULT"

# Same test with the C/Wasm emulator loading the tar at the kernel's fixed
# initrd address.
test-initrd-wasm: kernel.elf initrd.tar
	$(MAKE) -C ../emulator web/rv64.wasm
	node ../emulator/test/boot.mjs kernel.elf 200000000 \
		"\r\rcat from-initrd.txt\rexit 7\r" \
		'initrd: 12 file(s) loaded\nhello from a real initrd tar\nprocess: all processes exited' \
		initrd.tar

# Full closure test: build a riscv64 TCC with this repository's TCC, place
# that compiler, its own sources, and its musl build environment in the real
# tar initrd, then have it compile and link TCC again inside this kernel.
# Only needs BusyBox from the ordinary fixture set -- kmain.c's product
# boot execve()s real argv {"ash","-i"} straight against it (no
# separate wrapper ELF needed any more), and kernel.elf boots straight
# to an interactive shell (no checkpoint chain to feed proc_test/
# proc_fork_test/etc to first), so test/build-selfhost-initrd.sh no
# longer packages the rest of the P4-P10 fixture set either.
selfhost-initrd tcc-initrd: user_test/busybox_riscv64.elf user_test/fetch_riscv64.elf
	$(MAKE) -C ../compiler clean
	$(MAKE) -C ../compiler TARGET=riscv64 stage1
	sh test/build-selfhost-initrd.sh .. $@.tar

RISCV64_SELFHOST_INPUT := \r\rls\rls tcc-src\rcd tcc-src\rpwd\rcd /\rash /selfhost.sh\rexit 7\r

test-selfhost: kernel.elf
	$(MAKE) selfhost-initrd
	python3 test/boot_test.py kernel.elf \
		--qemu-binary qemu-system-riscv64 \
		--qemu-arg=-M --qemu-arg=virt \
		--qemu-arg=-bios --qemu-arg=default \
		--qemu-arg=-cpu --qemu-arg=rv64,sstc=true \
		--qemu-arg=-device --qemu-arg=loader,file=selfhost-initrd.tar,addr=0x84000000 \
		--mem 128 --timeout 30 \
		--stdin-input "$(RISCV64_SELFHOST_INPUT)" \
		--must-contain "SELFHOST: stage1 compiling TCC" \
		--must-contain "tcc-src" \
		--must-contain "musl" \
		--must-contain "/tcc-src" \
		--must-contain "SELFHOST: stage2 runs" \
		--must-contain "tcc version 0.9.27-rv64min (riscv64 Linux)" \
		--must-contain "hello from stage2 tcc" \
		--must-contain "SELFHOST: complete" \
		--must-contain "process: all processes exited" \
		--must-not-contain "FATAL" \
		--must-not-contain "PAGE FAULT"

endif

clean:
	rm -f kernel.elf kernel-checkpoints.elf arch/risc/riscv64_kmain_checkpoints.o \
		arch/risc/riscv64_task.o test/riscv64_checkpoints.o initrd.tar \
		tcc-initrd.tar selfhost-initrd.tar $(RISCV64_USER_ELFS) \
		user_test/user_test_riscv64.elf \
		arch/i386/kmain_checkpoints.o test/i386_checkpoints.o \
		initrd-i386-checkpoints.tar initrd-i386.tar initrd-i386-bb.tar \
		selfhost-initrd-i386.tar user_test/hello_i386.elf \
		user_test/proc_fork_test_i386.elf $(OBJS)
