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

/* Checkpoint 8 change: was 0x80300000, a "generous 1MB of headroom"
 * that held up fine through P1-P7 (~2000 lines of kernel plus a
 * handful of small ELF test payloads, all comfortably under 1MB
 * combined) but stopped being generous the moment a *real* embedded
 * payload showed up -- mm/ramfs.c embedding busybox itself (~740KB
 * alone) pushes the kernel image well past the old boundary, which
 * would silently overlap the scratch region below with live kernel
 * .data (busybox's own bytes landing on top of the trap dispatch
 * pointer/trapframe/trap stack, or vice versa -- neither the linker
 * nor anything at boot has any way to notice, since nothing in this
 * kernel's own sections is ever placed *at* these addresses by the
 * linker; they're pure runtime-pointer conventions). Caught by
 * computing the actual numbers (kernel_end plus busybox's real size)
 * before embedding it, not by booting into the corruption first --
 * see mm/ramfs.c's own comment. Moved out to a full 4MB past
 * KERNEL_LINK_BASE, comfortable headroom over the ~1.2MB this
 * checkpoint actually uses and generous room for whatever gets added
 * after it.
 *
 * Everything below is a hardcoded absolute address, not a linker
 * symbol -- required by arch/riscv64_boot.S and
 * arch/riscv64_trap_entry.S, which (being raw .long-encoded machine
 * code, not real assembly TCC can relocate -- see riscv64_boot.S's
 * comment) can only reference *numeric constants*, never symbols;
 * both were regenerated (arch/gen_riscv64_asm.sh, real riscv64-as,
 * not hand-computed opcodes) when this moved, diffed against their
 * previous versions to confirm only the address-loading instructions
 * changed. Laid out on separate pages for clarity, not packed
 * tightly -- we have 128MB of RAM and none of this needs to be
 * dense. */
#define RV64_SCRATCH_BASE      0x80600000UL

/* [0x80600000, 0x80604000): general S-mode stack -- used by
 * riscv64_entry.c's trampoline (the very first C code that runs) and
 * by every kernel task afterward until the scheduler installs its own
 * per-task stacks (sched/riscv64_task.c). sp starts at the top. */
#define RV64_BOOT_STACK_TOP    (RV64_SCRATCH_BASE + 0x4000UL) /* 0x80604000 */

/* One 8-byte slot at 0x80605000: void (*)(struct regs *), written
 * once by arch/riscv64_trap.c's trap_init() before any trap can
 * occur, read by arch/riscv64_trap_entry.S on every trap. This is how
 * raw asm calls into compiled C without needing a symbol relocation
 * it can't have -- populated at *runtime* by ordinary (fully
 * relocatable) C code, not baked in at generation time. */
#define RV64_TRAP_DISPATCH_PTR 0x80605000UL

/* One 8-byte slot at 0x80605008, right after RV64_TRAP_DISPATCH_PTR:
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
#define RV64_CURRENT_KSTACK_PTR 0x80605008UL

/* struct regs (arch/riscv64_trap.h): 35 8-byte fields = 280 bytes, at
 * 0x80606000. One global instance -- safe because this kernel never
 * nests traps (matches i386's own cli-until-iret non-reentrancy). */
#define RV64_TRAPFRAME_BASE    0x80606000UL

/* [0x80607000, 0x8060b000): dedicated trap-handling stack, switched
 * to unconditionally on every trap regardless of whether it
 * interrupted S-mode or U-mode code -- simpler than distinguishing
 * origins, and safe for the same non-nesting reason above (see
 * arch/riscv64_trap_entry.S). sp starts at the top. */
#define RV64_TRAP_STACK_TOP    0x8060b000UL

/* checkpoint 13: a real, separate boot module -- an uncompressed tar
 * archive (mm/tar.c), loaded at this fixed physical address by
 * whatever's actually booting the kernel (QEMU's `-device
 * loader,file=...,addr=...` -- kernel/Makefile's own test target --
 * or emulator/js/boot.js's equivalent second fetch()+loadBytes()),
 * *not* baked into kernel.elf the way every other embedded payload in
 * this kernel is. 64MB in from RV64_RAM_BASE: comfortably past
 * kernel_end (a few MB at most) and the scratch region above, with
 * still-generous headroom below RV64_MEM_TOP (128MB total) for
 * whatever the archive actually contains. Not a linker symbol or a
 * hand-written .S constant -- read only from ordinary, fully
 * relocatable C (riscv64_kmain.c, mm/tar.c) -- so unlike the addresses
 * above, this one didn't need arch/gen_riscv64_asm.sh regeneration
 * when picked; it's hardcoded for the same "we fully control the
 * QEMU invocation" reason as everything else in this file, not
 * because anything *requires* it to be a raw immediate. */
#define RV64_INITRD_BASE       0x84000000UL

/* Reserved via pmm_reserve_range() (riscv64_kmain.c, right alongside
 * the scratch region above) so nothing else can be handed this memory
 * by pmm_alloc_page()/pmm_alloc_contiguous() before mm/tar.c's own
 * tar_load_initrd() gets to read it -- real bytes actually sitting in
 * real RAM at boot, not a value anything computes. The tar format's own
 * two-all-zero-block end marker (mm/tar.c's own comment) means the
 * *real* archive can be far smaller than this without anything needing
 * to know its exact size in advance -- this is just how far
 * tar_load_initrd() is willing to keep scanning before giving up, a
 * safety bound, not a size QEMU/boot.js need to match.
 *
 * checkpoint 14: bumped from 4MB to 16MB -- a self-hosting TCC payload
 * (its own source + musl-riscv64's headers/libc.a/crt objects + a
 * prebuilt riscv64 tcc binary) measures ~5-6MB; 16MB is real headroom
 * above that measured size, not a guess. Leaves [0x84000000, 0x85000000)
 * reserved, comfortably under RV64_MEM_TOP (0x88000000) with plenty
 * left for the kernel's own use (dynamic-file backing storage included --
 * mm/ramfs.c's tar-loaded files are copied into pmm_alloc_contiguous()'d
 * pages elsewhere, not read in place from this reserved region). */
#define RV64_INITRD_MAX_SIZE   0x1000000UL /* 16MB */

#endif
