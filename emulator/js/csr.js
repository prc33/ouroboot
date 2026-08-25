'use strict';
/* Named CSR addresses this emulator actually implements -- matches
 * exactly what kernel/arch/riscv64_trap.c, riscv64_timer.c, and
 * riscv64_paging.c use (cross-check against those files, not the full
 * privileged spec's CSR list -- see docs/emulator-plan.md's E4: no
 * M-mode CSRs at all, since the kernel never touches any). */

var CSR = {
	SSTATUS: 0x100,
	SIE: 0x104,
	STVEC: 0x105,
	SSCRATCH: 0x140,
	SEPC: 0x141,
	SCAUSE: 0x142,
	STVAL: 0x143,
	SIP: 0x144,
	SATP: 0x180,
	TIME: 0xc01,     /* unprivileged, read-only, Zicsr `rdtime` alias */
	STIMECMP: 0x14d, /* Sstc */
};

var SSTATUS_SIE  = 1 << 1;
var SSTATUS_SPIE = 1 << 5;
var SSTATUS_SPP  = 1 << 8;
var SSTATUS_SUM  = 1 << 18;

var SIE_STIE = 1 << 5; /* supervisor timer interrupt enable */
var SIP_STIP = 1 << 5; /* supervisor timer interrupt pending */

module.exports = { CSR: CSR, SSTATUS_SIE: SSTATUS_SIE,
	SSTATUS_SPIE: SSTATUS_SPIE, SSTATUS_SPP: SSTATUS_SPP,
	SSTATUS_SUM: SSTATUS_SUM, SIE_STIE: SIE_STIE, SIP_STIP: SIP_STIP };
