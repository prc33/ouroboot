/* TCC's linker doesn't emit an _end symbol automatically (checked:
 * absent from the symbol table). This file supplies one -- but only
 * correctly if it's the LAST object file on the link command line,
 * since TCC's linker (no section GC, no reordering -- confirmed
 * during the TCC strip-down) concatenates each section in exactly
 * link-file order. See Makefile: kend.o is last in OBJS. */
.section .bss
.globl kernel_end
kernel_end:
