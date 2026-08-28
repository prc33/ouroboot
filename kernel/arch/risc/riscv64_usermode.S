.globl enter_usermode
enter_usermode:
	csrw sepc,a0
	csrr t0,sstatus
	li t1,2
	and t2,t0,t1
	slli t2,t2,0x4
	li t1,288
	not t1,t1
	and t0,t0,t1
	or t0,t0,t2
	csrw sstatus,t0
	mv sp,a1
	sret
