.globl _start
_start:
	lui sp,0x20
	addiw sp,sp,385 # sp now 0x20181
	slli sp,sp,0xe # sp = 0x80604000 (RV64_BOOT_STACK_TOP)
	call _riscv64_entry
