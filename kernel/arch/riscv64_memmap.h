#ifndef RISCV64_MEMMAP_H
#define RISCV64_MEMMAP_H

/* Fixed physical addresses this kernel hardcodes rather than discovers
 * (from the devicetree, or via a real allocator) -- deliberate
 * simplification matching this kernel's existing style (see mm/pmm.c's
 * fixed MAX_MEMORY_MB, and kmain.c's fixed test virtual addresses like
 * 0x400000/0xB0000000): we fully control the QEMU invocation this
 * kernel is tested under (kernel/test/boot_test.py), so "hardcode what
 * we control" is a safe simplification here, not a guess. Not parsing
 * the devicetree also sidesteps needing any dynamic-memory bootstrap.
 *
 * QEMU's riscv64 `virt` machine: RAM starts at 0x80000000. OpenSBI
 * (loaded via `-bios default`) occupies the first part of it and
 * hands off to our kernel in S-mode; the standard payload convention
 * (confirmed empirically -- see docs/riscv-port-findings.md) is
 * 0x80200000, 2MB in.
 */
#define KERNEL_LINK_BASE       0x80200000UL

/* QEMU's riscv64 virt machine RAM base, and the top of RAM this
 * kernel is tested with -- kernel/Makefile's test target passes
 * `-m 128M` to QEMU, so [RV64_RAM_BASE, RV64_MEM_TOP) is exactly what
 * we're told is available; hardcoded rather than parsed from the
 * devicetree, same "we fully control the QEMU invocation" reasoning
 * as above. */
#define RV64_RAM_BASE          0x80000000UL
#define RV64_MEM_TOP           0x88000000UL /* RV64_RAM_BASE + 128MB */

/* A generous 1MB of headroom between the kernel image (link base) and
 * this block is more than this ~2000-line kernel will ever need (the
 * i386 kernel.elf, doing equivalent work, is a few hundred KB at
 * most). Everything below is a hardcoded absolute address, not a
 * linker symbol -- required by arch/riscv64_boot.S and
 * arch/riscv64_trap_entry.S, which (being raw .long-encoded machine
 * code, not real assembly TCC can relocate -- see riscv64_boot.S's
 * comment) can only reference *numeric constants*, never symbols.
 * Laid out on separate pages for clarity, not packed tightly -- we
 * have 128MB of RAM and none of this needs to be dense. */
#define RV64_SCRATCH_BASE      0x80300000UL

/* [0x80300000, 0x80304000): general S-mode stack -- used by
 * riscv64_entry.c's trampoline (the very first C code that runs) and
 * by every kernel task afterward until the scheduler installs its own
 * per-task stacks (sched/riscv64_task.c). sp starts at the top. */
#define RV64_BOOT_STACK_TOP    (RV64_SCRATCH_BASE + 0x4000UL) /* 0x80304000 */

/* One 8-byte slot at 0x80305000: void (*)(struct regs *), written
 * once by arch/riscv64_trap.c's trap_init() before any trap can
 * occur, read by arch/riscv64_trap_entry.S on every trap. This is how
 * raw asm calls into compiled C without needing a symbol relocation
 * it can't have -- populated at *runtime* by ordinary (fully
 * relocatable) C code, not baked in at generation time. */
#define RV64_TRAP_DISPATCH_PTR 0x80305000UL

/* One 8-byte slot at 0x80305008, right after RV64_TRAP_DISPATCH_PTR:
 * the *top* of whichever process's kernel stack is current, read by
 * arch/riscv64_trap_entry.S on every trap instead of the single fixed
 * RV64_TRAP_STACK_TOP earlier checkpoints used. sched/riscv64_process.c
 * writes it every time a different process is about to run in
 * U-mode -- see that file's comment for why a single shared trap
 * stack stopped being safe once more than one process can genuinely
 * block mid-syscall (sched_yield, and later wait4/read): trap
 * handling for whichever process traps next now needs to happen ON
 * that same process's own kernel stack, so a block-and-resume deep in
 * one process's syscall handler can't be clobbered by a second
 * process trapping while the first is still suspended there. Same
 * "runtime-populated pointer slot, not a baked-in immediate" trick as
 * RV64_TRAP_DISPATCH_PTR, for the same reason (no relocation support
 * for hand-written .S files). */
#define RV64_CURRENT_KSTACK_PTR 0x80305008UL

/* struct regs (arch/riscv64_trap.h): 35 8-byte fields = 280 bytes, at
 * 0x80306000. One global instance -- safe because this kernel never
 * nests traps (matches i386's own cli-until-iret non-reentrancy). */
#define RV64_TRAPFRAME_BASE    0x80306000UL

/* [0x80307000, 0x8030b000): dedicated trap-handling stack, switched
 * to unconditionally on every trap regardless of whether it
 * interrupted S-mode or U-mode code -- simpler than distinguishing
 * origins, and safe for the same non-nesting reason above (see
 * arch/riscv64_trap_entry.S). sp starts at the top. */
#define RV64_TRAP_STACK_TOP    0x8030b000UL

#endif
