/* Minimal ring3 payload, no libc: write(1, msg, len); exit(0).
 * Linux i386 syscall convention (matches what our syscall dispatcher
 * implements, and what real musl-compiled binaries will also use --
 * D4): eax=number, ebx/ecx/edx/esi/edi/ebp=args 1-6, int $0x80. */
.globl _start
_start:
	movl $4, %eax        /* SYS_write */
	movl $1, %ebx         /* fd = 1 */
	movl $msg, %ecx
	movl $msglen, %edx
	int $0x80

	movl $1, %eax         /* SYS_exit */
	movl $0, %ebx
	int $0x80

	/* should never reach here */
1:	jmp 1b

msg:
	.ascii "hello from ring3 via int 0x80\n"
msglen = . - msg
