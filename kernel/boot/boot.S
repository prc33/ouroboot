/* Multiboot v1 entry point. QEMU's -kernel loader (and any Multiboot-
 * compliant loader) reads this header, drops the CPU into 32-bit
 * protected mode with a flat unpaged address space, and jumps to
 * _start with EAX=multiboot magic, EBX=pointer to the multiboot info
 * struct. No BIOS calls, no real mode, no bootloader of our own -- see
 * plan decision D8. Checksum is precomputed (see docs/boot-header.txt)
 * rather than trusting the assembler's constant folder for it. */

.align 4
multiboot_header:
	.long 0x1BADB002   /* magic */
	.long 0x00000002   /* flags: bit 1 = MEMINFO (want mem_lower/mem_upper) */
	.long 0xE4524FFC   /* checksum: -(magic+flags) mod 2^32 */

.globl _start
_start:
	movl $stack_top, %esp
	push %ebx          /* multiboot info pointer -> kmain's 2nd arg */
	push %eax          /* multiboot magic        -> kmain's 1st arg */
	call kmain
halt:
	cli
	hlt
	jmp halt

.section .bss
	.align 16
stack_bottom:
	.skip 16384
stack_top:
