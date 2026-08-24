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
        mm/pmm.o mm/riscv64_paging.o mm/elf.o \
        sched/riscv64_task.o sched/riscv64_switch_context.o \
        sched/riscv64_process.o \
        arch/riscv64_kend.o
else
$(error unknown ARCH '$(ARCH)' -- expected i386 or riscv64)
endif

.PHONY: all test test-js clean

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
# Shared between `test` (QEMU) and `test-js` (our own emulator/js/ --
# see docs/emulator-plan.md's P2) so the two never silently drift
# apart -- both are asserting the exact same checkpoints, just under
# two different implementations of the same ISA.
RISCV64_TEST_ASSERTIONS := \
	--must-contain "boot: entered kmain in S-mode" \
	--must-contain "trap: stvec installed" \
	--must-contain "breakpoint: ebreak handled and resumed OK" \
	--must-contain "pmm:" \
	--must-contain "paging: Sv39 identity-mapped" \
	--must-contain "COW test: parent=1 child=2 OK" \
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
	--must-not-contain "FATAL" \
	--must-not-contain "UNHANDLED EXCEPTION" \
	--must-not-contain "UNHANDLED INTERRUPT" \
	--must-not-contain "PAGE FAULT"

test: kernel.elf
	python3 test/boot_test.py kernel.elf \
		--qemu-binary qemu-system-riscv64 \
		--qemu-arg=-M --qemu-arg=virt \
		--qemu-arg=-bios --qemu-arg=default \
		--qemu-arg=-cpu --qemu-arg=rv64,sstc=true \
		$(RISCV64_TEST_ASSERTIONS)

# Our own from-scratch emulator (emulator/js/) instead of QEMU -- see
# docs/emulator-plan.md. --time-advance speeds up the virtual Sstc
# clock relative to real instructions retired, so the scheduler test's
# wfi-spin-wait doesn't need tens of millions of idle steps to see 10
# ticks; --max-instructions is generous headroom for a first-pass
# unoptimized interpreter (the full suite runs in ~10-20s as of P2).
test-js: kernel.elf
	node ../emulator/js/boot.js kernel.elf \
		--max-instructions 200000000 \
		--time-advance 100 \
		$(RISCV64_TEST_ASSERTIONS)
endif

clean:
	rm -f kernel.elf $(OBJS)
