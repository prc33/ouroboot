# Kernel build. Everything here is built with OUR OWN stripped TCC --
# not host gcc -- per the plan's hard constraint that kernel/ and
# emulator/ must self-host. See ../tcc/.

TCC     := ../compiler/tcc
TCCDIR  := ../compiler
TCCINC  := $(TCCDIR)/include

CFLAGS  := -B$(TCCDIR) -I$(TCCINC) -I. -Wall -nostdinc -fno-stack-protector -ffreestanding
LDFLAGS := -B$(TCCDIR) -nostdlib -static -Wl,-Ttext=0x100000

OBJS := boot/boot.o kmain.o drivers/serial.o drivers/kprintf.o \
        arch/gdt.o arch/idt.o arch/isr_stubs.o arch/pic.o arch/pit.o \
        arch/usermode.o arch/syscall_stub.o arch/syscall.o \
        mm/pmm.o mm/paging.o mm/elf.o sched/task.o sched/switch_context.o \
        arch/kend.o

.PHONY: all test clean

all: kernel.elf

boot/boot.o: boot/boot.S
	$(TCC) $(CFLAGS) -c -o $@ $<

arch/isr_stubs.o: arch/isr_stubs.S
	$(TCC) $(CFLAGS) -c -o $@ $<

arch/kend.o: arch/kend.S
	$(TCC) $(CFLAGS) -c -o $@ $<

sched/switch_context.o: sched/switch_context.S
	$(TCC) $(CFLAGS) -c -o $@ $<

arch/usermode.o: arch/usermode.S
	$(TCC) $(CFLAGS) -c -o $@ $<

arch/syscall_stub.o: arch/syscall_stub.S
	$(TCC) $(CFLAGS) -c -o $@ $<

%.o: %.c kernel.h
	$(TCC) $(CFLAGS) -c -o $@ $<

kernel.elf: $(OBJS)
	$(TCC) $(LDFLAGS) -o $@ $(OBJS)

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

clean:
	rm -f kernel.elf $(OBJS)
