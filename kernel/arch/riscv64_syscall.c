/* Syscall dispatch, Linux riscv64 ABI: a7=number, a0-a5=args 1-6,
 * return value in a0. Same "derived from a real strace, not guessed"
 * methodology as arch/syscall.c -- these are exactly the syscalls
 * `qemu-riscv64-static -strace` showed our own musl-linked riscv64
 * hello binary (from the compiler-port work) actually calling on its
 * way to main() and back: set_tid_address, brk, mmap, munmap, ioctl,
 * writev, exit_group. See docs/riscv-port-findings.md.
 *
 * Notably shorter than i386's list: riscv64 needs no
 * set_thread_area -- TLS is just the `tp` register, set directly by
 * musl's own _start (arch/riscv64/crt_arch.h), no syscall involved.
 * mmap (222) replaces i386's separate mmap2 -- riscv64 only has the
 * one, offset in bytes not pages (irrelevant here, anonymous-only).
 *
 * Same "single set of file-static globals, not yet a real per-process
 * struct" simplification as i386 -- still only ever one ring3 context
 * at a time. */
#include "kernel.h"
#include "riscv64_trap.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "sched/process.h"

#define SYS_ioctl             29
#define SYS_sched_yield      124
#define SYS_write              64
#define SYS_writev              66
#define SYS_exit                  93
#define SYS_exit_group              94
#define SYS_set_tid_address           96
#define SYS_brk                         214
#define SYS_munmap                        215
#define SYS_mmap                            222

#define MAP_FIXED  0x10
#define MAP_ANON   0x20

#define PROT_NONE  0x0

#define EBADF   9
#define EINVAL  22
#define ENOTTY  25
#define ENOMEM  12
#define ENOSYS  38

/* --- process address-space bookkeeping (see file comment) --- */
#define BRK_BASE   0x40000000UL
#define MMAP_BASE  0x60000000UL

static unsigned long brk_current = 0;
static unsigned long next_mmap_addr = MMAP_BASE;

static unsigned long page_round_up(unsigned long x) {
	return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1UL);
}

static void sys_write_impl(unsigned long fd, const char *buf, unsigned long count) {
	if (fd == 1 || fd == 2) {
		for (unsigned long i = 0; i < count; i++)
			serial_putc(buf[i]);
	}
}

static void sys_write(struct regs *r) {
	unsigned long fd = r->a0;
	if (fd != 1 && fd != 2) {
		r->a0 = (unsigned long)-EBADF;
		return;
	}
	sys_write_impl(fd, (const char *)r->a1, r->a2);
	r->a0 = r->a2;
}

/* struct iovec { void *iov_base; size_t iov_len; } -- 16 bytes on riscv64 */
static void sys_writev(struct regs *r) {
	unsigned long fd = r->a0;
	unsigned long *iov = (unsigned long *)r->a1;
	unsigned long iovcnt = r->a2;
	if (fd != 1 && fd != 2) {
		r->a0 = (unsigned long)-EBADF;
		return;
	}
	unsigned long total = 0;
	for (unsigned long i = 0; i < iovcnt; i++) {
		const char *base = (const char *)iov[i * 2];
		unsigned long len = iov[i * 2 + 1];
		sys_write_impl(fd, base, len);
		total += len;
	}
	r->a0 = total;
}

static void sys_exit_impl(struct regs *r, const char *which) {
	kprintf("\n[process exited with code %ld]\n", (long)r->a0);
	kprintf("ring3 test OK\n");
	kprintf("%s\n", which);
}

static void sys_exit(struct regs *r) {
	sys_exit_impl(r, "P5 checkpoint 1 OK");
	run_elf_test();
}

/* checkpoint 6: once sched/riscv64_process.c's process_run() has
 * started (process_mode_active()), exit_group means "this one process
 * is done", not "halt everything" -- process_exit_current() reschedules
 * to whatever else is still runnable, or halts only once nothing is
 * left. Before that point (every P4/P5 checkpoint), exit_group keeps
 * its original one-shot meaning unchanged: conclude P5 checkpoint 2
 * and hand off to run_process_test() (riscv64_kmain.c) instead of
 * halting outright -- this is the one call site that used to be the
 * kernel's final halt and is now where checkpoint 6 actually starts. */
static void sys_exit_group(struct regs *r) {
	if (process_mode_active())
		process_exit_current((int)r->a0); /* noreturn */
	sys_exit_impl(r, "P5 checkpoint 2 OK");
	run_process_test();
}

static void sys_sched_yield(struct regs *r) {
	process_schedule();
	r->a0 = 0;
}

static void sys_brk(struct regs *r) {
	unsigned long requested = r->a0;

	if (brk_current == 0)
		brk_current = BRK_BASE; /* first call: musl queries with NULL first */

	if (requested == 0) {
		r->a0 = brk_current;
		return;
	}
	if (requested > brk_current) {
		unsigned long old_top = page_round_up(brk_current);
		unsigned long new_top = page_round_up(requested);
		for (unsigned long va = old_top; va < new_top; va += PAGE_SIZE) {
			unsigned long phys = pmm_alloc_page();
			if (!phys) {
				r->a0 = brk_current; /* out of memory: brk unchanged, per Linux semantics */
				return;
			}
			paging_map_page(va, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
		}
	}
	/* shrinking: update accounting only, don't reclaim pages yet --
	 * documented simplification, see file comment */
	brk_current = requested;
	r->a0 = brk_current;
}

static void sys_mmap(struct regs *r) {
	unsigned long addr = r->a0;
	unsigned long length = r->a1;
	/* r->a2 = prot, r->a3 = flags, r->a4 = fd, r->a5 = offset */
	unsigned long prot = r->a2;
	unsigned long flags = r->a3;

	/* Real Linux returns EINVAL for a zero-length mmap; musl's static
	 * TLS setup relies on exactly that (a program needing no TLS
	 * block, like this test binary, computes a genuinely zero-sized
	 * request and expects it to fail so its fallback path can skip
	 * the reservation). Returning a fake "success" here instead --
	 * which the pre-this-fix code did, falling through to the general
	 * path below where the length-0 mapping loop below correctly maps
	 * *nothing* but still reports success -- handed musl an address
	 * it believed was safely backed and wasn't, which page-faulted on
	 * the first real write. Found by tracing exactly which mmap call
	 * returned the address that later faulted. */
	if (length == 0) {
		r->a0 = (unsigned long)-EINVAL;
		return;
	}

	unsigned long len = page_round_up(length);

	if ((flags & MAP_FIXED) && prot == PROT_NONE) {
		/* musl's TLS/stack guard-page reservation: reserve the range
		 * (never map it, so any real access correctly faults) rather
		 * than actually backing it with memory. */
		r->a0 = addr;
		return;
	}

	unsigned long base;
	if (flags & MAP_FIXED) {
		base = addr;
	} else {
		base = next_mmap_addr;
		next_mmap_addr += len;
	}

	for (unsigned long va = base; va < base + len; va += PAGE_SIZE) {
		unsigned long phys = pmm_alloc_page();
		if (!phys) {
			r->a0 = (unsigned long)-ENOMEM;
			return;
		}
		paging_map_page(va, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
	}
	r->a0 = base;
}

static void sys_munmap(struct regs *r) {
	unsigned long addr = r->a0;
	unsigned long len = page_round_up(r->a1);
	for (unsigned long va = addr; va < addr + len; va += PAGE_SIZE) {
		unsigned long phys = paging_get_phys(va);
		if (phys) {
			paging_map_page(va, 0, 0); /* clear PTE_PRESENT */
			pmm_free_page(phys & ~0xFFFUL);
		}
	}
	r->a0 = 0;
}

#define TIOCGWINSZ 0x5413

static void sys_ioctl(struct regs *r) {
	unsigned long fd = r->a0;
	unsigned long req = r->a1;
	if ((fd == 1 || fd == 2) && req == TIOCGWINSZ) {
		/* honest answer: there's no real tty layer yet (P7), and our
		 * fd 1/2 is a serial line, not a terminal -- ENOTTY is what a
		 * non-tty stdout genuinely returns here on real Linux too. */
		r->a0 = (unsigned long)-ENOTTY;
		return;
	}
	r->a0 = (unsigned long)-EINVAL;
}

static void sys_set_tid_address(struct regs *r) {
	/* Real semantics (clear this address + futex-wake on thread exit)
	 * don't matter yet -- no threads, no futex. Just needs to succeed
	 * and return a plausible tid; every syscall from this one process
	 * is "tid 1" for now. */
	(void)r;
	r->a0 = 1;
}

static void syscall_dispatch(struct regs *r) {
	switch (r->a7) {
	case SYS_write:            sys_write(r); return;
	case SYS_writev:           sys_writev(r); return;
	case SYS_exit:              sys_exit(r); return;
	case SYS_exit_group:        sys_exit_group(r); return;
	case SYS_brk:                sys_brk(r); return;
	case SYS_mmap:                 sys_mmap(r); return;
	case SYS_munmap:                 sys_munmap(r); return;
	case SYS_ioctl:                    sys_ioctl(r); return;
	case SYS_sched_yield:                 sys_sched_yield(r); return;
	case SYS_set_tid_address:            sys_set_tid_address(r); return;
	default:
		kprintf("FATAL: unimplemented syscall %lu\n", r->a7);
		r->a0 = (unsigned long)-ENOSYS;
	}
}

void syscall_init(void) {
	syscall_set_handler(syscall_dispatch);
	kprintf("syscall: dispatch installed (write, writev, exit, exit_group, "
		"brk, mmap, munmap, ioctl, sched_yield, set_tid_address)\n");
}
