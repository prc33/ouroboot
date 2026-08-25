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
# anything outside this project's own toolchain (that cross-assembler
# is a discovery tool used offline, by hand, to generate the raw
# riscv64_*.S files checked into arch/ and boot/ -- see
# arch/gen_riscv64_asm.sh).

ARCH ?= i386

TCC     := ../compiler/tcc
TCCDIR  := ../compiler
TCCINC  := $(TCCDIR)/include

CFLAGS  := -B$(TCCDIR) -I$(TCCINC) -I. -Wall -nostdinc -fno-stack-protector -ffreestanding

ifeq ($(ARCH),i386)
LDFLAGS := -B$(TCCDIR) -nostdlib -static -Wl,-Ttext=0x100000
OBJS := boot/boot.o kmain.o drivers/serial.o drivers/kprintf.o \
        arch/gdt.o arch/idt.o arch/isr_stubs.o arch/pic.o arch/pit.o \
        arch/usermode.o arch/syscall_stub.o arch/syscall.o \
        mm/pmm.o mm/paging.o mm/elf.o sched/task.o sched/switch_context.o \
        arch/kend.o
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
# boot/riscv64_boot.o MUST be first (its hardcoded jump target assumes
# it occupies exactly the first 0x1000 bytes of the image -- see its
# own comment) and riscv64_entry.o must be immediately next (its
# _riscv64_entry must be the very first thing linked after that page).
# arch/riscv64_kend.o must be last, same reason as i386's arch/kend.o.
OBJS := boot/riscv64_boot.o riscv64_entry.o riscv64_kmain.o \
        drivers/riscv64_serial.o drivers/kprintf.o \
        arch/riscv64_trap_entry.o arch/riscv64_trap.o arch/riscv64_timer.o \
        arch/riscv64_usermode.o arch/riscv64_syscall.o \
        mm/pmm.o mm/riscv64_paging.o mm/elf.o mm/ramfs.o mm/tar.o \
        sched/riscv64_task.o sched/riscv64_switch_context.o \
        sched/riscv64_process.o \
        arch/riscv64_kend.o
else
$(error unknown ARCH '$(ARCH)' -- expected i386 or riscv64)
endif

.PHONY: all test test-wasm shell-wasm test-initrd test-initrd-wasm \
	test-selfhost tcc-initrd selfhost-initrd clean

all: kernel.elf

boot/boot.o: boot/boot.S
	$(TCC) $(CFLAGS) -c -o $@ $<

boot/riscv64_boot.o: boot/riscv64_boot.S
	$(TCC) $(CFLAGS) -c -o $@ $<

arch/isr_stubs.o: arch/isr_stubs.S
	$(TCC) $(CFLAGS) -c -o $@ $<

arch/kend.o: arch/kend.S
	$(TCC) $(CFLAGS) -c -o $@ $<

arch/riscv64_kend.o: arch/riscv64_kend.S
	$(TCC) $(CFLAGS) -c -o $@ $<

sched/switch_context.o: sched/switch_context.S
	$(TCC) $(CFLAGS) -c -o $@ $<

sched/riscv64_switch_context.o: sched/riscv64_switch_context.S
	$(TCC) $(CFLAGS) -c -o $@ $<

arch/usermode.o: arch/usermode.S
	$(TCC) $(CFLAGS) -c -o $@ $<

arch/riscv64_usermode.o: arch/riscv64_usermode.S
	$(TCC) $(CFLAGS) -c -o $@ $<

arch/syscall_stub.o: arch/syscall_stub.S
	$(TCC) $(CFLAGS) -c -o $@ $<

arch/riscv64_trap_entry.o: arch/riscv64_trap_entry.S
	$(TCC) $(CFLAGS) -c -o $@ $<

%.o: %.c kernel.h
	$(TCC) $(CFLAGS) -c -o $@ $<

kernel.elf: $(OBJS)
	$(TCC) $(LDFLAGS) -o $@ $(OBJS)

ifeq ($(ARCH),i386)
test: kernel.elf
	python3 test/boot_test.py kernel.elf \
		--must-contain "multiboot magic OK" \
		--must-contain "mem_lower" \
		--must-contain "gdt: loaded" \
		--must-contain "idt: loaded" \
		--must-contain "pic: remapped" \
		--must-contain "breakpoint: int3 handled and resumed OK" \
		--must-contain "pmm:" \
		--must-contain "paging: identity-mapped" \
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
# stdin (kernel/riscv64_kmain.c's run_interactive_test) -- exactly
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
# xterm.js's term.onData in the browser demo -- see arch/riscv64_syscall.c's
# sys_read for the ICRNL-by-hand translation this exercises). Using
# \n here instead would silently stop testing the exact byte a real
# human's Enter key -- or a real terminal attached to a real QEMU
# `-serial stdio` in interactive mode -- actually produces.
#
# Leading \r\r: real QEMU's `-serial stdio` loses exactly the first 2
# bytes fed in over a pipe, consistently -- confirmed real (traced
# down to arch/riscv64_syscall.c's sys_read never even seeing them)
# and confirmed harmless for real interactive use (a human can't type
# before this kernel's own boot sequence, which is what's actually
# consuming those 2 byte-times, finishes -- see drivers/riscv64_serial.c's
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
# by arch/riscv64_syscall.c's sys_openat/sys_getdents64 (see their own
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
# 32MB of *real* RAM despite arch/riscv64_memmap.h's RV64_MEM_TOP
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
test: kernel.elf initrd.tar
	python3 test/boot_test.py kernel.elf \
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
test-wasm: kernel.elf initrd.tar
	$(MAKE) -C ../emulator web/rv64.wasm
	node ../emulator/test/boot.mjs kernel.elf 340000000 \
		"$(RISCV64_INTERACTIVE_INPUT)" 'P10 checkpoint OK' initrd.tar

# Direct product boot used for interactive development and the emulator's
# startup-time budget. It runs the exact same P10 process/BusyBox path while
# leaving the full historical checkpoint sequence in `test-wasm` untouched.
SHELL_OBJS := $(patsubst riscv64_kmain.o,riscv64_kmain_shell.o,$(OBJS))

riscv64_kmain_shell.o: riscv64_kmain.c kernel.h
	$(TCC) $(CFLAGS) -DKERNEL_DIRECT_SHELL -c -o $@ $<

kernel-shell.elf: $(SHELL_OBJS)
	$(TCC) $(LDFLAGS) -o $@ $(SHELL_OBJS)

# Covers both properties this target exists for: real interactive use
# (RISCV64_INTERACTIVE_INPUT's `echo`/`ls`/writable-file/`exit` round
# trip actually runs and produces "hello ash" + a real ls listing + a
# real created-and-read-back file + a clean process-table drain, not
# just a prompt appearing -- reaching "built-in shell (ash)" alone
# doesn't prove typed input is ever read).
shell-wasm: kernel-shell.elf initrd.tar
	$(MAKE) -C ../emulator test-shell

# The ordinary explicit filesystem image: BusyBox, the process-test fixtures,
# and one plain data file. The host tar packages it; QEMU or the Wasm host puts
# it at RV64_INITRD_BASE. The kernel contains no file payload bytes.
initrd.tar: test/build-initrd.sh test/initrd-fixture/from-initrd.txt \
		test/initrd-fixture/greeting test/initrd-fixture/test.sh \
		user_test/busybox_riscv64.elf user_test/exec_target_riscv64.elf
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
		--must-contain "initrd: 5 file(s) loaded" \
		--must-contain "hello from a real initrd tar" \
		--must-contain "process: all processes exited" \
		--must-contain "P10 checkpoint OK" \
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
		'initrd: 5 file(s) loaded\nhello from a real initrd tar\nP10 checkpoint OK' \
		initrd.tar

# Full closure test: build a riscv64 TCC with this repository's TCC, place
# that compiler, its own sources, and its musl build environment in the real
# tar initrd, then have it compile and link TCC again inside this kernel.
selfhost-initrd:
	$(MAKE) -C ../compiler clean
	$(MAKE) -C ../compiler TARGET=riscv64 stage1
	sh test/build-selfhost-initrd.sh .. selfhost-initrd.tar

# Small interactive image: the ordinary BusyBox filesystem plus a runnable
# stage-1 compiler, without the much larger source/header closure payload.
tcc-initrd:
	$(MAKE) -C ../compiler clean
	$(MAKE) -C ../compiler TARGET=riscv64 stage1
	sh test/build-initrd.sh .. tcc-initrd.tar ../compiler/stage1/tcc

RISCV64_SELFHOST_INPUT := \r\recho SELFHOST: stage1 compiling TCC\r/tcc -B/tcc-src -I/tcc-src -I/tcc-src/include -I/musl/obj/include -I/musl/include -I/musl/arch/riscv64 -I/musl/arch/generic -nostdinc -DTCC_TARGET_RISCV64 -c -o /tcc-stage2.o /tcc-src/tcc.c\r/tcc -B/tcc-src -static -nostdinc -nostdlib -o /tcc-stage2 /musl/lib/crt1.o /musl/lib/crti.o /tcc-stage2.o /musl/lib/libc.a /tcc-src/libtcc1.a /musl/lib/crtn.o\recho SELFHOST: stage2 runs\r/tcc-stage2 -v\recho SELFHOST: stage2 compiling program\r/tcc-stage2 -B/tcc-src -I/tcc-src/include -I/musl/obj/include -I/musl/include -I/musl/arch/riscv64 -I/musl/arch/generic -nostdinc -static -nostdlib -o /selfhost-hello /musl/lib/crt1.o /musl/lib/crti.o /selfhost-hello.c /musl/lib/libc.a /tcc-src/libtcc1.a /musl/lib/crtn.o\r/selfhost-hello\recho SELFHOST: complete\rexit 7\r

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
		--must-contain "SELFHOST: stage2 runs" \
		--must-contain "tcc version 0.9.27-rv64min (riscv64 Linux)" \
		--must-contain "hello from stage2 tcc" \
		--must-contain "SELFHOST: complete" \
		--must-contain "P10 checkpoint OK" \
		--must-not-contain "FATAL" \
		--must-not-contain "PAGE FAULT"

endif

clean:
	rm -f kernel.elf kernel-shell.elf riscv64_kmain_shell.o initrd.tar \
		tcc-initrd.tar selfhost-initrd.tar $(OBJS)
