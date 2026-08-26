/* Syscall dispatch, Linux i386 ABI (plan decision D4): eax=number,
 * ebx/ecx/edx/esi/edi/ebp=args 1-6, return value in eax.
 *
 * The set implemented here isn't guessed -- it's exactly what
 * `qemu-i386-static -strace` showed our own musl-linked hello binary
 * (from the earlier TCC/musl spike) actually calling on its way to
 * main() and back: set_thread_area, set_tid_address, brk, mmap2,
 * munmap, ioctl(TIOCGWINSZ), writev, exit_group. See
 * docs/kernel-p5-findings.md for the full trace and how each call was
 * mapped to an implementation here.
 *
 * Per-process state (brk_current, next_mmap_addr, tls state) is still
 * a single set of file-static globals, not yet a real per-process
 * struct -- there's still only ever one ring3 context at a time. Real
 * multi-process support (P6) needs to move this into a process table;
 * noted rather than silently assumed away.
 *
 * syscall_common.h/.c (kernel root, not this ARCH's own directory):
 * handler logic that turned out, once compared against riscv64's own
 * arch/risc/riscv64_syscall.c, to be identical or that should have
 * been -- see that header's own comment for two real bugs (unzeroed
 * brk/mmap pages, a missing zero-length-mmap check) found and fixed
 * this way. This file still owns every syscall *number* and every
 * register read (eax/ebx/ecx/...) -- the Linux ABI genuinely differs
 * there, not just historically. */
#include "kernel.h"
#include "idt.h"
#include "syscall_common.h"

#define SYS_exit             1
#define SYS_write             4
#define SYS_brk               45
#define SYS_ioctl             54
#define SYS_munmap            91
#define SYS_writev            146
#define SYS_mmap2             192
#define SYS_set_thread_area   243
#define SYS_exit_group        252
#define SYS_set_tid_address   258

#define MAP_FIXED  0x10
#define MAP_ANON   0x20

#define PROT_NONE  0x0

#define EBADF   9
#define EINVAL  22
#define ENOMEM  12
#define ENOSYS  38

/* --- process address-space bookkeeping (see file comment) --- */
#define BRK_BASE   0x40000000u
#define MMAP_BASE  0x60000000u

static unsigned int brk_current = 0;
static unsigned int next_mmap_addr = MMAP_BASE;

static void sys_write(struct regs *r) {
	unsigned int fd = r->ebx;
	if (fd != 1 && fd != 2) {
		r->eax = (unsigned int)-EBADF;
		return;
	}
	syscall_write_raw(fd, (const char *)(unsigned long)r->ecx, r->edx);
	r->eax = r->edx;
}

/* struct iovec { void *iov_base; size_t iov_len; } -- 8 bytes on i386 */
static void sys_writev(struct regs *r) {
	unsigned int fd = r->ebx;
	unsigned int *iov = (unsigned int *)(unsigned long)r->ecx;
	unsigned int iovcnt = r->edx;
	if (fd != 1 && fd != 2) {
		r->eax = (unsigned int)-EBADF;
		return;
	}
	unsigned int total = 0;
	for (unsigned int i = 0; i < iovcnt; i++) {
		const char *base = (const char *)(unsigned long)iov[i * 2];
		unsigned int len = iov[i * 2 + 1];
		syscall_write_raw(fd, base, len);
		total += len;
	}
	r->eax = total;
}

static void sys_exit_impl(struct regs *r, const char *which) {
	kprintf("\n[process exited with code %d]\n", (int)r->ebx);
	kprintf("ring3 test OK\n");
	kprintf("%s\n", which);
}

static void sys_exit(struct regs *r) {
	sys_exit_impl(r, "P5 checkpoint 1 OK");
	/* chains into the next checkpoint (real ELF loader + real musl
	 * binary) rather than halting -- see kernel.h/kmain.c. Checkpoint-
	 * chaining scaffolding, not real process-exit semantics; a kernel
	 * with real process management wouldn't do this. */
	run_elf_test();
}

static void sys_exit_group(struct regs *r) {
	sys_exit_impl(r, "P5 checkpoint 2 OK");
	kprintf("halting.\n");
	for (;;) __asm__ volatile ("cli\n hlt");
}

static void sys_brk(struct regs *r) {
	unsigned int requested = r->ebx;

	if (brk_current == 0)
		brk_current = BRK_BASE; /* first call: musl queries with NULL first */

	if (requested == 0) {
		r->eax = brk_current;
		return;
	}
	if (requested > brk_current) {
		unsigned int old_top = (unsigned int)syscall_page_round_up(brk_current);
		unsigned int new_top = (unsigned int)syscall_page_round_up(requested);
		if (!syscall_grow_pages(old_top, new_top)) {
			r->eax = brk_current; /* out of memory: brk unchanged, per Linux semantics */
			return;
		}
	}
	/* shrinking: update accounting only, don't reclaim pages yet --
	 * documented simplification, see file comment */
	brk_current = requested;
	r->eax = brk_current;
}

static void sys_mmap2(struct regs *r) {
	unsigned int addr = r->ebx;
	unsigned int length = r->ecx;
	/* r->edx = prot, r->esi = flags, r->edi = fd, r->ebp = pgoffset */
	unsigned int prot = r->edx;
	unsigned int flags = r->esi;

	/* Real Linux returns EINVAL for a zero-length mmap -- see
	 * syscall_common.h's own header for why this specific check
	 * matters (riscv64's own sys_mmap already had it; this file
	 * didn't, until compared side by side to build the shared
	 * version this now calls). */
	if (length == 0) {
		r->eax = (unsigned int)-EINVAL;
		return;
	}

	unsigned int len = (unsigned int)syscall_page_round_up(length);

	if ((flags & MAP_FIXED) && prot == PROT_NONE) {
		/* musl's TLS/stack guard-page reservation: reserve the range
		 * (never map it, so any real access correctly faults) rather
		 * than actually backing it with memory. */
		r->eax = addr;
		return;
	}

	unsigned int base;
	if (flags & MAP_FIXED) {
		base = addr;
	} else {
		base = next_mmap_addr;
		next_mmap_addr += len;
	}

	if (!syscall_grow_pages(base, base + len)) {
		r->eax = (unsigned int)-ENOMEM;
		return;
	}
	r->eax = base;
}

static void sys_munmap(struct regs *r) {
	unsigned int addr = r->ebx;
	unsigned int len = (unsigned int)syscall_page_round_up(r->ecx);
	syscall_munmap_pages(addr, len);
	r->eax = 0;
}

static void sys_ioctl(struct regs *r) {
	r->eax = (unsigned int)syscall_ioctl_check(r->ebx, r->ecx);
}

/* struct user_desc, Linux's real layout (see docs/kernel-p5-findings.md
 * for how this was derived from musl's __set_thread_area.s):
 *   u32 entry_number; u32 base_addr; u32 limit; u32 flags_bitfield;
 * We don't decode the flags bitfield -- every real caller (musl) sends
 * the same standard "present, 32-bit, page-granular, full 4GB, usable"
 * descriptor, so gdt_set_tls_entry always builds exactly that, keyed
 * only off base_addr. */
static void sys_set_thread_area(struct regs *r) {
	unsigned int *desc = (unsigned int *)(unsigned long)r->ebx;
	unsigned int base_addr = desc[1];

	/* Deliberately NOT validating entry_number (desc[0]) here -- see
	 * docs/kernel-p5-findings.md. musl's __set_thread_area.s computes
	 * its cached "-1" sentinel via a call-then-add-label-difference
	 * trick spanning the .text/.data gap, and TCC's linker resolves
	 * that specific relocation 3 bytes short of the real target
	 * (confirmed directly: linked a minimal reproduction, checked the
	 * actual .data address against the relocated immediate by hand).
	 * The value musl reads back is garbage as a result. Since this
	 * kernel only ever supports one TLS user at a time anyway and
	 * always hands out slot 6 regardless of what was requested, the
	 * simplest correct fix is to just not require entry_number to be
	 * meaningful -- every real call only cares that base_addr is
	 * right, which it is. */
	int slot = 6;
	gdt_set_tls_entry(slot, base_addr);
	desc[0] = (unsigned int)slot; /* kernel writes the allocated slot back */
	r->eax = 0;
}

static void sys_set_tid_address(struct regs *r) {
	/* Real semantics (clear this address + futex-wake on thread exit)
	 * don't matter yet -- no threads, no futex. Just needs to succeed
	 * and return a plausible tid; every syscall from this one process
	 * is "tid 1" for now. */
	(void)r;
	r->eax = 1;
}

static void syscall_dispatch(struct regs *r) {
	switch (r->eax) {
	case SYS_write:            sys_write(r); return;
	case SYS_writev:           sys_writev(r); return;
	case SYS_exit:              sys_exit(r); return;
	case SYS_exit_group:        sys_exit_group(r); return;
	case SYS_brk:                sys_brk(r); return;
	case SYS_mmap2:               sys_mmap2(r); return;
	case SYS_munmap:               sys_munmap(r); return;
	case SYS_ioctl:                  sys_ioctl(r); return;
	case SYS_set_thread_area:          sys_set_thread_area(r); return;
	case SYS_set_tid_address:            sys_set_tid_address(r); return;
	default:
		kprintf("FATAL: unimplemented syscall %u\n", r->eax);
		r->eax = (unsigned int)-ENOSYS;
	}
}

void syscall_init(void) {
	syscall_set_handler(syscall_dispatch);
	kprintf("syscall: dispatch installed (write, writev, exit, exit_group, "
		"brk, mmap2, munmap, ioctl, set_thread_area, set_tid_address)\n");
}
