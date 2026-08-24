'use strict';
/* RV64IM + Zicsr + the privileged subset kernel/ actually uses
 * (sret/ecall/ebreak/wfi/sfence.vma) + Sstc, plus a deliberately narrow
 * slice of F/D (see "F/D support" below) -- see docs/emulator-plan.md's
 * "Derive the ISA subset". No compressed, no atomics: still absent from
 * both kernel/kernel.elf's and compiler/stage1/tcc's own histograms,
 * reconfirmed alongside the F/D work below, not just carried over.
 *
 * No M-mode at all (docs/emulator-plan.md's E4): every trap, from
 * either S-mode or U-mode, goes straight to stvec, matching how this
 * specific kernel is actually used (it never touches an M-mode CSR).
 *
 * F/D support (P4, docs/emulator-plan.md): the kernel itself uses none
 * of this (reconfirmed: its own histogram is still exactly RV64IM +
 * Zicsr + privileged + Sstc), but compiler/stage1/tcc -- a real
 * self-hosted userspace binary, riscv64-gen.c's own `double` codegen --
 * does. Its own mnemonic histogram (`riscv64-linux-gnu-objdump -d
 * stage1/tcc | ...`, same technique as P0) is exactly: fld, flw, fsd,
 * fsw, fadd.d, fmul.d, fmul.s, fdiv.d, fdiv.s, fcvt.s.d, fcvt.s.w,
 * fmv.d, fmv.s, fmv.x.d -- 14 opcodes, no fsub, no comparisons
 * (feq/flt/fle), no fsqrt, no fclass, no fmin/fmax. Implemented to
 * exactly that list, not the full ~75-instruction F/D extension --
 * matches this project's "narrow, explicit, and safe beats broad and
 * unverified" bias (riscv64-gen.c's own stated philosophy). Anything
 * else in the LOAD-FP/STORE-FP/OP-FP opcode space throws
 * IllegalInstruction with the funct7/funct3 that was missing, so a
 * genuinely new requirement surfaces as a clear trap, not silent wrong
 * behavior -- extend when that actually happens, per the same
 * histogram-then-implement method used for the base ISA.
 *
 * Rounding mode (the funct3 field on arithmetic F/D ops, "rm") is
 * ignored -- every occurrence in the histogram above uses funct3=111
 * (DYN, "use frm CSR"), and frm is never written by this binary either,
 * so it's always the default RNE. JS's own float64 arithmetic and
 * Math.fround() both round-to-nearest-even already, so this falls out
 * for free rather than needing an frm-aware software rounder.
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

/* Scratch buffer for float64<->bits and float32<->bits reinterpretation
 * (JS has no direct "punning" operator -- a DataView over a shared
 * ArrayBuffer is the standard way to get IEEE754 bit patterns). Module-
 * level and reused across calls since it's write-then-immediately-read,
 * never held across a yield point. */
const fConvBuf = new ArrayBuffer(8);
const fConvView = new DataView(fConvBuf);

class Cpu {
	constructor(mem) {
		this.mem = mem;
		this.x = new Array(32).fill(0n);
		/* 32 float/double registers, stored as raw 64-bit bit patterns
		 * (BigUint64Array) -- the single source of truth, same as real
		 * hardware. Single-precision values are NaN-boxed (upper 32 bits
		 * all 1s) per the D-extension spec; getF32/setF32 below implement
		 * that, getF64/setF64 the plain double case, getFBits/setFBits
		 * the raw-move case fmv.x.d/fld/fsd/etc. need. */
		this.f = new BigUint64Array(32);
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

	getFBits(i) { return this.f[i]; }
	setFBits(i, bits) { this.f[i] = BigInt.asUintN(64, bits); }

	getF64(i) {
		fConvView.setBigUint64(0, this.f[i], true);
		return fConvView.getFloat64(0, true);
	}
	setF64(i, val) {
		fConvView.setFloat64(0, val, true);
		this.f[i] = fConvView.getBigUint64(0, true);
	}

	/* NaN-boxing (D-ext spec 12.2): a single-precision value in a 64-bit
	 * F register is only valid if the upper 32 bits are all 1s; anything
	 * else reads back as the canonical 32-bit quiet NaN. Not expected to
	 * ever trigger here (nothing in the histogram writes f-regs any way
	 * but setF32/setF64 above), but it's part of the spec's actual
	 * semantics, not an edge case worth skipping. */
	getF32(i) {
		if ((this.f[i] >> 32n) !== 0xffffffffn) return NaN;
		fConvView.setUint32(0, Number(this.f[i] & 0xffffffffn), true);
		return fConvView.getFloat32(0, true);
	}
	setF32(i, val) {
		fConvView.setFloat32(0, val, true);
		const bits32 = BigInt(fConvView.getUint32(0, true));
		this.f[i] = (0xffffffffn << 32n) | bits32;
	}

	/* fsgnj.s/fsgnj.d: rs1's magnitude, rs2's sign bit -- operates on raw
	 * bits, not the numeric value (so it's correct for NaNs too, unlike
	 * doing this via Math.abs()/multiply). fmv.s/fmv.d (rs2===rs1, a
	 * self-copy) fall out of this for free, matching how the real ISA
	 * defines them as pseudo-instructions rather than separate opcodes. */
	rs2SignOnF32(rs1, rs2) {
		const mag = this.getFBits(rs1) & 0x7fffffffn;
		const sign = this.getFBits(rs2) & 0x80000000n;
		fConvView.setUint32(0, Number(mag | sign), true);
		return fConvView.getFloat32(0, true);
	}
	rs2SignOnF64(rs1, rs2) {
		const mag = this.getFBits(rs1) & 0x7fffffffffffffffn;
		const sign = this.getFBits(rs2) & 0x8000000000000000n;
		fConvView.setBigUint64(0, mag | sign, true);
		return fConvView.getFloat64(0, true);
	}

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
			case 0x07: { /* LOAD-FP: flw/fld -- only these two appear in the histogram */
				const imm = sext(BigInt(inst >>> 20), 12);
				const addr = (this.getX(rs1) + imm) & MASK64;
				try {
					switch (funct3) {
						case 2: /* flw -- raw 32-bit load, NaN-boxed into the 64-bit f-reg */
							this.setFBits(rd, (0xffffffffn << 32n) | (this.load(addr, 4) & 0xffffffffn));
							break;
						case 3: /* fld -- raw 64-bit load */
							this.setFBits(rd, this.load(addr, 8));
							break;
						default: throw new IllegalInstruction(inst);
					}
				} catch (e) { e.accessKind = 'r'; throw e; }
				break;
			}
			case 0x27: { /* STORE-FP: fsw/fsd */
				const imm = sext((BigInt((inst >>> 7) & 0x1f)) | (BigInt((inst >>> 25) & 0x7f) << 5n), 12);
				const addr = (this.getX(rs1) + imm) & MASK64;
				try {
					switch (funct3) {
						case 2: this.store(addr, 4, this.getFBits(rs2) & 0xffffffffn); break; /* fsw */
						case 3: this.store(addr, 8, this.getFBits(rs2)); break;                /* fsd */
						default: throw new IllegalInstruction(inst);
					}
				} catch (e) { e.accessKind = 'w'; throw e; }
				break;
			}
			case 0x53: { /* OP-FP: see the header comment for exactly which
			                funct7/funct3 combos are implemented and why. */
				switch (funct7) {
					case 0x01: this.setF64(rd, this.getF64(rs1) + this.getF64(rs2)); break; /* fadd.d */
					case 0x08: this.setF32(rd, Math.fround(this.getF32(rs1) * this.getF32(rs2))); break; /* fmul.s */
					case 0x09: this.setF64(rd, this.getF64(rs1) * this.getF64(rs2)); break; /* fmul.d */
					case 0x0c: this.setF32(rd, Math.fround(this.getF32(rs1) / this.getF32(rs2))); break; /* fdiv.s */
					case 0x0d: this.setF64(rd, this.getF64(rs1) / this.getF64(rs2)); break; /* fdiv.d */
					case 0x10: /* FSGNJ.S family -- only funct3=0 (fsgnj.s) needed; fmv.s is fsgnj.s rs1,rs1 */
						if (funct3 !== 0) throw new IllegalInstruction(inst);
						this.setF32(rd, this.rs2SignOnF32(rs1, rs2));
						break;
					case 0x11: /* FSGNJ.D family -- only funct3=0 (fsgnj.d) needed; fmv.d is fsgnj.d rs1,rs1 */
						if (funct3 !== 0) throw new IllegalInstruction(inst);
						this.setF64(rd, this.rs2SignOnF64(rs1, rs2));
						break;
					case 0x20: /* FCVT.S.<fmt>: only rs2=1 (fcvt.s.d) needed */
						if (rs2 !== 1) throw new IllegalInstruction(inst);
						this.setF32(rd, Math.fround(this.getF64(rs1)));
						break;
					case 0x68: /* FCVT.S.<int>: only rs2=0 (fcvt.s.w, signed 32-bit int) needed */
						if (rs2 !== 0) throw new IllegalInstruction(inst);
						this.setF32(rd, Math.fround(Number(BigInt.asIntN(32, this.getX(rs1) & 0xffffffffn))));
						break;
					case 0x71: /* FMV.X.D / FCLASS.D: only funct3=0 (fmv.x.d, raw bit move) needed */
						if (rs2 !== 0 || funct3 !== 0) throw new IllegalInstruction(inst);
						this.setX(rd, this.getFBits(rs1));
						break;
					default:
						throw new IllegalInstruction(inst);
				}
				break;
			}
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
