'use strict';
/* RV64IM + Zicsr + the privileged subset kernel/ actually uses
 * (sret/ecall/ebreak/wfi/sfence.vma) + Sstc. No F/D, no compressed, no
 * atomics -- see docs/emulator-plan.md's "Derive the ISA subset"
 * (Milestone A); this is exactly what a mnemonic histogram of
 * kernel/kernel.elf showed, not a guess.
 *
 * No M-mode at all (docs/emulator-plan.md's E4): every trap, from
 * either S-mode or U-mode, goes straight to stvec, matching how this
 * specific kernel is actually used (it never touches an M-mode CSR).
 */

const { CSR, SSTATUS_SIE, SSTATUS_SPIE, SSTATUS_SPP, SSTATUS_SUM, SIE_STIE, SIP_STIP } = require('./csr');
const { translate, PageFault } = require('./mmu');
const { BusError } = require('./memory');

const MASK64 = (1n << 64n) - 1n;

/* Sign-extend a `bits`-wide field (given as a BigInt of that width) to
 * a full 64-bit two's-complement value, returned as unsigned BigInt
 * (i.e. already masked to 64 bits, ready to store in a register). */
function sext(value, bits) {
	const b = BigInt(bits);
	const signBit = 1n << (b - 1n);
	if (value & signBit)
		return (value | (MASK64 << b)) & MASK64;
	return value & ((1n << b) - 1n);
}

class IllegalInstruction extends Error {
	constructor(inst) {
		super(`illegal instruction ${inst.toString(16)}`);
	}
}

class Cpu {
	constructor(mem) {
		this.mem = mem;
		this.x = new Array(32).fill(0n);
		this.pc = 0n;
		this.priv = 'S'; /* E4: start directly in S-mode, no OpenSBI/M-mode */
		this.csr = new Map();
		for (const addr of Object.values(CSR))
			this.csr.set(addr, 0n);
		this.time = 0n;
		this.halted = false; /* set by wfi; cleared when an interrupt becomes pending */
		this.instret = 0n;
	}

	getX(i) { return i === 0 ? 0n : this.x[i]; }
	setX(i, v) { if (i !== 0) this.x[i] = v & MASK64; }

	getCsr(addr) {
		if (addr === CSR.TIME) return this.time;
		return this.csr.get(addr) ?? 0n;
	}
	setCsr(addr, v) {
		this.csr.set(addr, v & MASK64);
	}

	/* --- memory access, MMU + fault translation --- */

	translateAddr(vaddr, access) {
		const satp = this.getCsr(CSR.SATP);
		const sum = (this.getCsr(CSR.SSTATUS) & SSTATUS_SUM) !== 0n;
		return translate(this.mem, satp, vaddr, access, this.priv, sum);
	}

	fetch32(vaddr) {
		const paddr = this.translateAddr(vaddr, 'x');
		return this.mem.read(paddr, 4);
	}

	load(vaddr, size, access = 'r') {
		const paddr = this.translateAddr(vaddr, access);
		return this.mem.read(paddr, size);
	}

	store(vaddr, size, value) {
		const paddr = this.translateAddr(vaddr, 'w');
		this.mem.write(paddr, size, value);
	}

	/* --- traps --- */

	raiseTrap(cause, tval, isInterrupt) {
		const causeVal = isInterrupt ? (cause | (1n << 63n)) : cause;
		this.setCsr(CSR.SEPC, this.pc);
		this.setCsr(CSR.SCAUSE, causeVal);
		this.setCsr(CSR.STVAL, tval);

		let sstatus = this.getCsr(CSR.SSTATUS);
		sstatus = this.priv === 'S' ? (sstatus | SSTATUS_SPP) : (sstatus & ~SSTATUS_SPP);
		const sie = (sstatus & SSTATUS_SIE) ? 1n : 0n;
		sstatus = (sstatus & ~SSTATUS_SPIE) | (sie << 5n);
		sstatus &= ~SSTATUS_SIE;
		this.setCsr(CSR.SSTATUS, sstatus);

		this.priv = 'S';
		this.pc = this.getCsr(CSR.STVEC) & ~0x3n; /* direct mode only -- low 2 bits are the mode field, kernel always uses 0 */
		this.halted = false;
	}

	/* Maps a caught access exception to the right scause and re-raises
	 * as a trap. `access` is 'x'/'r'/'w', matching mmu.js's own
	 * vocabulary, so the three access-fault/page-fault cause codes
	 * (which differ only in a fixed offset per access type) fall out
	 * of one table instead of three near-duplicate catch blocks. */
	handleAccessFault(e, access) {
		const causes = {
			x: { bus: 1n, page: 12n },
			r: { bus: 5n, page: 13n },
			w: { bus: 7n, page: 15n },
		}[access];
		if (e instanceof PageFault) {
			this.raiseTrap(causes.page, e.vaddr, false);
			return true;
		}
		if (e instanceof BusError) {
			this.raiseTrap(causes.bus, e.addr, false);
			return true;
		}
		return false;
	}

	checkInterrupts() {
		if (this.time >= this.getCsr(CSR.STIMECMP))
			this.setCsr(CSR.SIP, this.getCsr(CSR.SIP) | SIP_STIP);
		else
			this.setCsr(CSR.SIP, this.getCsr(CSR.SIP) & ~SIP_STIP);

		const sstatus = this.getCsr(CSR.SSTATUS);
		const globallyEnabled = this.priv === 'U' || (this.priv === 'S' && (sstatus & SSTATUS_SIE));
		const pending = this.getCsr(CSR.SIP) & this.getCsr(CSR.SIE) & SIP_STIP;
		if (globallyEnabled && pending) {
			this.halted = false;
			this.raiseTrap(5n, 0n, true); /* supervisor timer interrupt */
			return true;
		}
		return false;
	}

	/* --- main loop --- */

	step(timeAdvance) {
		this.time += timeAdvance;
		if (this.checkInterrupts())
			return;
		if (this.halted)
			return;

		let inst;
		try {
			inst = this.fetch32(this.pc);
		} catch (e) {
			if (this.handleAccessFault(e, 'x')) return;
			throw e;
		}

		try {
			this.execute(Number(inst));
		} catch (e) {
			if (e instanceof IllegalInstruction) { this.raiseTrap(2n, inst, false); return; }
			if (this.handleAccessFault(e, e.accessKind || 'r')) return;
			throw e;
		}
		this.instret++;
	}

	/* --- decode + execute --- */

	execute(inst) {
		const opcode = inst & 0x7f;
		const rd = (inst >>> 7) & 0x1f;
		const funct3 = (inst >>> 12) & 0x7;
		const rs1 = (inst >>> 15) & 0x1f;
		const rs2 = (inst >>> 20) & 0x1f;
		const funct7 = (inst >>> 25) & 0x7f;

		const pc = this.pc;
		let nextPc = pc + 4n;

		switch (opcode) {
			case 0x37: { /* LUI */
				this.setX(rd, sext(BigInt(inst & 0xfffff000) & 0xffffffffn, 32));
				break;
			}
			case 0x17: { /* AUIPC */
				const imm = sext(BigInt(inst & 0xfffff000) & 0xffffffffn, 32);
				this.setX(rd, (pc + imm) & MASK64);
				break;
			}
			case 0x6f: { /* JAL */
				const imm = sext(
					(BigInt((inst >>> 21) & 0x3ff) << 1n) |
					(BigInt((inst >>> 20) & 0x1) << 11n) |
					(BigInt((inst >>> 12) & 0xff) << 12n) |
					(BigInt((inst >>> 31) & 0x1) << 20n),
					21
				);
				this.setX(rd, nextPc);
				nextPc = (pc + imm) & MASK64;
				break;
			}
			case 0x67: { /* JALR */
				const imm = sext(BigInt(inst >>> 20), 12);
				const target = (this.getX(rs1) + imm) & ~1n & MASK64;
				this.setX(rd, nextPc);
				nextPc = target;
				break;
			}
			case 0x63: { /* BRANCH */
				const imm = sext(
					(BigInt((inst >>> 8) & 0xf) << 1n) |
					(BigInt((inst >>> 25) & 0x3f) << 5n) |
					(BigInt((inst >>> 7) & 0x1) << 11n) |
					(BigInt((inst >>> 31) & 0x1) << 12n),
					13
				);
				const a = this.getX(rs1), b = this.getX(rs2);
				const as = BigInt.asIntN(64, a), bs = BigInt.asIntN(64, b);
				let taken;
				switch (funct3) {
					case 0: taken = a === b; break;          /* beq */
					case 1: taken = a !== b; break;           /* bne */
					case 4: taken = as < bs; break;            /* blt */
					case 5: taken = as >= bs; break;            /* bge */
					case 6: taken = a < b; break;                /* bltu */
					case 7: taken = a >= b; break;                /* bgeu */
					default: throw new IllegalInstruction(inst);
				}
				if (taken) nextPc = (pc + imm) & MASK64;
				break;
			}
			case 0x03: { /* LOAD */
				const imm = sext(BigInt(inst >>> 20), 12);
				const addr = (this.getX(rs1) + imm) & MASK64;
				let v;
				try {
					switch (funct3) {
						case 0: v = sext(this.load(addr, 1), 8); break;   /* lb */
						case 1: v = sext(this.load(addr, 2), 16); break;  /* lh */
						case 2: v = sext(this.load(addr, 4), 32); break;  /* lw */
						case 3: v = this.load(addr, 8); break;             /* ld */
						case 4: v = this.load(addr, 1); break;              /* lbu */
						case 5: v = this.load(addr, 2); break;               /* lhu */
						case 6: v = this.load(addr, 4); break;                /* lwu */
						default: throw new IllegalInstruction(inst);
					}
				} catch (e) { e.accessKind = 'r'; throw e; }
				this.setX(rd, v);
				break;
			}
			case 0x23: { /* STORE */
				const imm = sext((BigInt((inst >>> 7) & 0x1f)) | (BigInt((inst >>> 25) & 0x7f) << 5n), 12);
				const addr = (this.getX(rs1) + imm) & MASK64;
				const v = this.getX(rs2);
				try {
					switch (funct3) {
						case 0: this.store(addr, 1, v); break; /* sb */
						case 1: this.store(addr, 2, v); break; /* sh */
						case 2: this.store(addr, 4, v); break; /* sw */
						case 3: this.store(addr, 8, v); break; /* sd */
						default: throw new IllegalInstruction(inst);
					}
				} catch (e) { e.accessKind = 'w'; throw e; }
				break;
			}
			case 0x13: { /* OP-IMM */
				const imm = sext(BigInt(inst >>> 20), 12);
				const a = this.getX(rs1);
				const shamt = BigInt(inst >>> 20) & 0x3fn;
				let v;
				switch (funct3) {
					case 0: v = (a + imm) & MASK64; break;                       /* addi */
					case 1: v = (a << shamt) & MASK64; break;                     /* slli */
					case 2: v = BigInt.asIntN(64, a) < BigInt.asIntN(64, imm) ? 1n : 0n; break; /* slti */
					case 3: v = a < (imm & MASK64) ? 1n : 0n; break;               /* sltiu */
					case 4: v = a ^ imm; break;                                     /* xori */
					case 5: {
						if (((inst >>> 25) & 0x7f) & 0x20) /* srai */
							v = BigInt.asUintN(64, BigInt.asIntN(64, a) >> shamt);
						else
							v = a >> shamt; /* srli */
						break;
					}
					case 6: v = a | imm; break; /* ori */
					case 7: v = a & imm; break; /* andi */
					default: throw new IllegalInstruction(inst);
				}
				this.setX(rd, v);
				break;
			}
			case 0x1b: { /* OP-IMM-32 */
				const a32 = BigInt.asIntN(32, this.getX(rs1) & 0xffffffffn);
				const shamt = BigInt(inst >>> 20) & 0x1fn;
				let v32;
				switch (funct3) {
					case 0: v32 = BigInt.asIntN(32, a32 + sext(BigInt(inst >>> 20), 12)); break; /* addiw */
					case 1: v32 = BigInt.asIntN(32, BigInt.asUintN(32, a32) << shamt); break;      /* slliw */
					case 5: {
						const u32 = BigInt.asUintN(32, a32);
						if (((inst >>> 25) & 0x7f) & 0x20)
							v32 = BigInt.asIntN(32, a32 >> shamt); /* sraiw */
						else
							v32 = BigInt.asIntN(32, u32 >> shamt);  /* srliw */
						break;
					}
					default: throw new IllegalInstruction(inst);
				}
				this.setX(rd, sext(BigInt.asUintN(32, v32), 32));
				break;
			}
			case 0x33: { /* OP (incl. M extension when funct7=1) */
				const a = this.getX(rs1), b = this.getX(rs2);
				const as = BigInt.asIntN(64, a), bs = BigInt.asIntN(64, b);
				let v;
				if (funct7 === 0x01) {
					switch (funct3) {
						case 0: v = (a * b) & MASK64; break;                                    /* mul */
						case 1: v = BigInt.asUintN(64, (as * bs) >> 64n); break;                  /* mulh */
						case 2: v = BigInt.asUintN(64, (as * b) >> 64n); break;                     /* mulhsu (b treated unsigned) */
						case 3: v = BigInt.asUintN(64, (a * b) >> 64n); break;                        /* mulhu */
						case 4: v = bs === 0n ? MASK64 : (bs === -1n && as === -(1n << 63n) ? as & MASK64 : BigInt.asUintN(64, as / bs)); break; /* div */
						case 5: v = b === 0n ? MASK64 : a / b; break;                                     /* divu */
						case 6: v = bs === 0n ? a : (bs === -1n && as === -(1n << 63n) ? 0n : BigInt.asUintN(64, as % bs)); break; /* rem */
						case 7: v = b === 0n ? a : a % b; break;                                            /* remu */
						default: throw new IllegalInstruction(inst);
					}
				} else {
					switch (funct3) {
						case 0: v = funct7 & 0x20 ? (a - b) & MASK64 : (a + b) & MASK64; break; /* sub/add */
						case 1: v = (a << (b & 0x3fn)) & MASK64; break;                           /* sll */
						case 2: v = as < bs ? 1n : 0n; break;                                       /* slt */
						case 3: v = a < b ? 1n : 0n; break;                                           /* sltu */
						case 4: v = a ^ b; break;                                                       /* xor */
						case 5: v = funct7 & 0x20 ? BigInt.asUintN(64, as >> (b & 0x3fn)) : a >> (b & 0x3fn); break; /* sra/srl */
						case 6: v = a | b; break;                                                          /* or */
						case 7: v = a & b; break;                                                           /* and */
						default: throw new IllegalInstruction(inst);
					}
				}
				this.setX(rd, v);
				break;
			}
			case 0x3b: { /* OP-32 (incl. M extension when funct7=1) */
				const a32 = BigInt.asIntN(32, this.getX(rs1) & 0xffffffffn);
				const b32 = BigInt.asIntN(32, this.getX(rs2) & 0xffffffffn);
				const bshamt = this.getX(rs2) & 0x1fn;
				let v32;
				if (funct7 === 0x01) {
					switch (funct3) {
						case 0: v32 = BigInt.asIntN(32, a32 * b32); break; /* mulw */
						case 4: v32 = b32 === 0n ? -1n : (b32 === -1n && a32 === -(1n << 31n) ? a32 : a32 / b32); break; /* divw */
						case 5: v32 = b32 === 0n ? -1n : BigInt.asIntN(32, BigInt.asUintN(32, a32) / BigInt.asUintN(32, b32)); break; /* divuw */
						case 6: v32 = b32 === 0n ? a32 : (b32 === -1n && a32 === -(1n << 31n) ? 0n : a32 % b32); break; /* remw */
						case 7: v32 = b32 === 0n ? a32 : BigInt.asIntN(32, BigInt.asUintN(32, a32) % BigInt.asUintN(32, b32)); break; /* remuw */
						default: throw new IllegalInstruction(inst);
					}
				} else {
					switch (funct3) {
						case 0: v32 = funct7 & 0x20 ? BigInt.asIntN(32, a32 - b32) : BigInt.asIntN(32, a32 + b32); break; /* subw/addw */
						case 1: v32 = BigInt.asIntN(32, BigInt.asUintN(32, a32) << bshamt); break;                          /* sllw */
						case 5: v32 = funct7 & 0x20 ? BigInt.asIntN(32, a32 >> bshamt) : BigInt.asIntN(32, BigInt.asUintN(32, a32) >> bshamt); break; /* sraw/srlw */
						default: throw new IllegalInstruction(inst);
					}
				}
				this.setX(rd, sext(BigInt.asUintN(32, v32), 32));
				break;
			}
			case 0x0f: /* MISC-MEM: fence, fence.i -- no-op (single-hart, no cache model) */
				break;
			case 0x73: { /* SYSTEM */
				nextPc = this.executeSystem(inst, rd, funct3, rs1, rs2, funct7, pc, nextPc);
				break;
			}
			default:
				throw new IllegalInstruction(inst);
		}

		this.pc = nextPc;
	}

	executeSystem(inst, rd, funct3, rs1, rs2, funct7, pc, nextPc) {
		if (funct3 === 0) {
			const imm12 = inst >>> 20;
			if (imm12 === 0x000) { /* ecall */
				const cause = this.priv === 'U' ? 8n : 9n;
				this.raiseTrap(cause, 0n, false);
				return this.pc; /* raiseTrap already set pc to stvec */
			}
			if (imm12 === 0x001) { /* ebreak */
				this.raiseTrap(3n, pc, false);
				return this.pc;
			}
			if (imm12 === 0x102) { /* sret */
				const sstatus = this.getCsr(CSR.SSTATUS);
				this.priv = (sstatus & SSTATUS_SPP) ? 'S' : 'U';
				let newStatus = (sstatus & SSTATUS_SPIE) ? (sstatus | SSTATUS_SIE) : (sstatus & ~SSTATUS_SIE);
				newStatus |= SSTATUS_SPIE; /* SPIE set to 1 per spec */
				newStatus &= ~SSTATUS_SPP; /* SPP reset to U (least privileged) */
				this.setCsr(CSR.SSTATUS, newStatus);
				return this.getCsr(CSR.SEPC);
			}
			if (imm12 === 0x105) { /* wfi */
				this.halted = true;
				return nextPc;
			}
			if ((inst >>> 25) === 0x09) { /* sfence.vma rs1,rs2 */
				return nextPc; /* no TLB modeled -- always a no-op is trivially correct */
			}
			throw new IllegalInstruction(inst);
		}

		/* Zicsr: csrrw/csrrs/csrrc (funct3 1-3), csrrwi/csrrsi/csrrci (5-7) */
		const csrAddr = (inst >>> 20) & 0xfff;
		const old = this.getCsr(csrAddr);
		const useImm = funct3 >= 5;
		const src = useImm ? BigInt(rs1) : this.getX(rs1);
		let updated;
		switch (funct3 & 0x3) {
			case 1: updated = src; break;                 /* csrrw(i) */
			case 2: updated = old | src; break;             /* csrrs(i) */
			case 3: updated = old & ~src; break;              /* csrrc(i) */
			default: throw new IllegalInstruction(inst);
		}
		/* rs1==0 (or zimm==0) for the read-modify-write forms means
		 * "don't write" per spec -- doesn't apply to csrrw(i), which
		 * always writes. */
		const isWriteForm = (funct3 & 0x3) === 1;
		if (isWriteForm || rs1 !== 0)
			this.setCsr(csrAddr, updated);
		this.setX(rd, old);
		return nextPc;
	}
}

module.exports = { Cpu, IllegalInstruction, sext };
