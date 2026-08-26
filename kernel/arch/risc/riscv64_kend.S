/* TCC's linker doesn't emit an _end symbol automatically (checked, i386
 * side: absent from the symbol table -- same is true here). This file
 * supplies one -- but only correctly if it's the LAST object file on
 * the link command line, since TCC's linker (no section GC, no
 * reordering) concatenates each section in exactly link-file order.
 * See kernel/Makefile: riscv64_kend.o is last in OBJS. Directives
 * only (.section/.globl/label) -- no instructions, so this needs no
 * cross-assembly at all, same as arch/i386/kend.S. */
.section .bss
.globl kernel_end
kernel_end:
