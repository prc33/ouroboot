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
const { BusError, RAM_BASE } = require('./memory');

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
		/* Typed storage applies RV64's modulo-2^64 register semantics on
		 * assignment, avoiding an explicit BigInt mask after every write. */
		this.x = new BigUint64Array(32);
		/* Exact Number mirror for address generation. Ouroboot maps all RAM,
		 * userspace, stacks, and MMIO below 4GB; full-width arithmetic still
		 * uses x above. */
		this.xNumber = new Float64Array(32);
		/* 32 float/double registers, stored as raw 64-bit bit patterns
		 * (BigUint64Array) -- the single source of truth, same as real
		 * hardware. Single-precision values are NaN-boxed (upper 32 bits
		 * all 1s) per the D-extension spec; getF32/setF32 below implement
		 * that, getF64/setF64 the plain double case, getFBits/setFBits
		 * the raw-move case fmv.x.d/fld/fsd/etc. need. */
		this.f = new BigUint64Array(32);
		/* Every executable mapping in this machine is below 4GB, where a JS
		 * Number is exact. GPRs remain full RV64 BigInts. */
		this.pc = 0;
		this.priv = 'S'; /* E4: start directly in S-mode, no OpenSBI/M-mode */
		this.csr = new Map();
		for (const addr of Object.values(CSR))
			this.csr.set(addr, 0n);
		/* Hot CSR mirrors. Fetch and interrupt checks touch these on every
		 * instruction; the Map remains the canonical interface for the rare
		 * guest CSR instructions. setCsr keeps both representations together. */
		this.satp = 0n;
		this.sstatus = 0n;
		this.sum = false;
		/* The modeled time cannot approach Number's exact-integer limit in a
		 * boot run. Keep the hot per-instruction clock as a Number and convert
		 * only when the guest explicitly reads the 64-bit TIME CSR. */
		this.time = 0;
		this.timecmp = 0;
		this.halted = false; /* set by wfi; cleared when an interrupt becomes pending */
		/* Single last-page translation for instruction fetch. Fetches
		 * normally stay on one 4KB page for thousands of instructions; walking
		 * all three Sv39 levels for every one dominated runtime. This deliberately
		 * tiny TLB captures that locality without a replacement policy or a
		 * general cache -- one entry, not one per access kind (loads/stores get
		 * their own, separate, much bigger cache below, since they actually
		 * range across many pages). satp and the permission-relevant privilege
		 * state are part of the tag, and sfence.vma invalidates it. */
		this.tlbVpage = -1n;
		this.tlbRamOffset = -1; /* the fetch fast path returns straight from RAM, so the physical
		                          * page itself is never needed again once this offset is cached */
		this.tlbSatp = 0n;
		this.tlbContext = -1;
		/* Loads and stores range across many pages, so each gets a tiny
		 * direct-mapped 256-page cache. The indexing rule is the replacement
		 * policy; there is no list, allocation, or search on the hot path.
		 * Ppage/satp are BigUint64Array, not plain Array-of-BigInt: a plain
		 * array stores each BigInt as a separate heap-boxed object, so every
		 * cache-miss fill would allocate; the typed array stores the raw
		 * 64-bit values inline instead. */
		this.dataTlbVpage = new Float64Array(512);
		this.dataTlbVpage.fill(-1);
		this.dataTlbPpage = new BigUint64Array(512);
		this.dataTlbSatp = new BigUint64Array(512);
		this.dataTlbContext = new Int8Array(512);
	}

	getX(i) { return i === 0 ? 0n : this.x[i]; }
	setX(i, v) {
		if (i !== 0) {
			this.x[i] = v;
			this.xNumber[i] = Number(this.x[i]);
		}
	}

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
		if (addr === CSR.TIME) return BigInt(this.time);
		if (addr === CSR.SATP) return this.satp;
		if (addr === CSR.SSTATUS) return this.sstatus;
		return this.csr.get(addr) ?? 0n;
	}
	setCsr(addr, v) {
		v &= MASK64;
		this.csr.set(addr, v);
		if (addr === CSR.SATP) this.satp = v;
		if (addr === CSR.SSTATUS) {
			this.sstatus = v;
			this.sum = (v & SSTATUS_SUM) !== 0n;
		}
		if (addr === CSR.STIMECMP) {
			this.timecmp = Number(v);
			if (this.timecmp === 0 || this.time < this.timecmp)
				this.csr.set(CSR.SIP, (this.csr.get(CSR.SIP) ?? 0n) & ~SIP_STIP);
		}
	}

	/* --- memory access, MMU + fault translation --- */

	translateAddr(vaddr, access) {
		const satp = this.satp;
		const sum = this.sum;
		const vpage = Math.floor(vaddr / 4096);
		const context = (this.priv === 'U' ? 1 : 0) | (sum ? 2 : 0);
		const slot = (vpage & 0xff) + (access === 'w' ? 256 : 0);
		if (this.dataTlbVpage[slot] === vpage &&
		    this.dataTlbSatp[slot] === satp &&
		    this.dataTlbContext[slot] === context)
			return this.dataTlbPpage[slot] | BigInt(vaddr & 0xfff);

		const paddr = translate(this.mem, satp, BigInt(vaddr), access, this.priv, sum);
		this.dataTlbVpage[slot] = vpage;
		this.dataTlbPpage[slot] = paddr & ~0xfffn;
		this.dataTlbSatp[slot] = satp;
		this.dataTlbContext[slot] = context;
		return paddr;
	}

	flushTlb() {
		this.tlbVpage = -1n;
		this.dataTlbVpage.fill(-1);
	}

	fetch32(vaddr) {
		const satp = this.satp;
		const sum = this.sum;
		const vpage = Math.floor(vaddr / 4096);
		const context = (this.priv === 'U' ? 1 : 0) | (sum ? 2 : 0);
		if (this.tlbVpage === vpage &&
		    this.tlbSatp === satp &&
		    this.tlbContext === context &&
		    this.tlbRamOffset >= 0)
			return this.mem.ram.getUint32(this.tlbRamOffset + (vaddr & 0xfff), true);

		const paddr = translate(this.mem, satp, BigInt(vaddr), 'x', this.priv, sum);
		this.tlbVpage = vpage;
		this.tlbSatp = satp;
		this.tlbContext = context;
		const off = paddr - RAM_BASE;
		if (off < 0n || off + 4n > this.mem.ramSize) {
			this.tlbRamOffset = -1;
			return Number(this.mem.read(paddr, 4));
		}
		this.tlbRamOffset = Number((paddr & ~0xfffn) - RAM_BASE);
		return this.mem.ram.getUint32(Number(off), true);
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
		this.setCsr(CSR.SEPC, BigInt(this.pc));
		this.setCsr(CSR.SCAUSE, causeVal);
		this.setCsr(CSR.STVAL, tval);

		let sstatus = this.getCsr(CSR.SSTATUS);
		sstatus = this.priv === 'S' ? (sstatus | SSTATUS_SPP) : (sstatus & ~SSTATUS_SPP);
		const sie = (sstatus & SSTATUS_SIE) ? 1n : 0n;
		sstatus = (sstatus & ~SSTATUS_SPIE) | (sie << 5n);
		sstatus &= ~SSTATUS_SIE;
		this.setCsr(CSR.SSTATUS, sstatus);

		this.priv = 'S';
		this.pc = Number(this.getCsr(CSR.STVEC) & ~0x3n); /* direct mode only -- low 2 bits are the mode field, kernel always uses 0 */
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
		if (this.timecmp === 0 || this.time < this.timecmp)
			return false;
		this.setCsr(CSR.SIP, this.getCsr(CSR.SIP) | SIP_STIP);

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
		this.run(1, timeAdvance);
	}

	/* Run a host-side batch without paying one JS method call per guest
	 * instruction. Interrupts and time still advance instruction by
	 * instruction inside the loop. */
	run(count, timeAdvance) {
		for (let i = 0; i < count; i++) {
			this.time += timeAdvance;
			if (this.timecmp !== 0 && this.time >= this.timecmp && this.checkInterrupts())
				continue;
			if (this.halted)
				continue;

			let inst;
			try {
				inst = this.fetch32(this.pc);
			} catch (e) {
				if (this.handleAccessFault(e, 'x')) continue;
				throw e;
			}

			try {
				this.execute(inst);
			} catch (e) {
				if (e instanceof IllegalInstruction) {
					this.raiseTrap(2n, BigInt(inst), false);
					continue;
				}
				if (this.handleAccessFault(e, e.accessKind || 'r')) continue;
				throw e;
			}
		}
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
		let nextPc = pc + 4;

		switch (opcode) {
			case 0x37: { /* LUI */
				this.setX(rd, BigInt((inst & 0xfffff000) | 0));
				break;
			}
			case 0x17: { /* AUIPC */
				const imm = BigInt((inst & 0xfffff000) | 0);
				this.setX(rd, BigInt(pc) + imm);
				break;
			}
			case 0x6f: { /* JAL */
				let imm = (((inst >>> 21) & 0x3ff) << 1) |
				          (((inst >>> 20) & 0x1) << 11) |
				          (((inst >>> 12) & 0xff) << 12) |
				          (((inst >>> 31) & 0x1) << 20);
				if (imm & 0x100000) imm -= 0x200000;
				this.setX(rd, BigInt(nextPc));
				nextPc = pc + imm;
				break;
			}
			case 0x67: { /* JALR */
				const imm = BigInt(inst >> 20);
				const target = (this.getX(rs1) + imm) & ~1n & MASK64;
				this.setX(rd, BigInt(nextPc));
				nextPc = Number(target);
				break;
			}
			case 0x63: { /* BRANCH */
				let imm = (((inst >>> 8) & 0xf) << 1) |
				          (((inst >>> 25) & 0x3f) << 5) |
				          (((inst >>> 7) & 0x1) << 11) |
				          (((inst >>> 31) & 0x1) << 12);
				if (imm & 0x1000) imm -= 0x2000;
				const a = this.getX(rs1), b = this.getX(rs2);
				let taken;
				switch (funct3) {
					case 0: taken = a === b; break;          /* beq */
					case 1: taken = a !== b; break;           /* bne */
					case 4: taken = BigInt.asIntN(64, a) < BigInt.asIntN(64, b); break; /* blt */
					case 5: taken = BigInt.asIntN(64, a) >= BigInt.asIntN(64, b); break; /* bge */
					case 6: taken = a < b; break;                /* bltu */
					case 7: taken = a >= b; break;                /* bgeu */
					default: throw new IllegalInstruction(inst);
				}
				if (taken) nextPc = pc + imm;
				break;
			}
			case 0x03: { /* LOAD */
				const addr = this.xNumber[rs1] + (inst >> 20);
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
				const addr = this.xNumber[rs1] + (((inst >> 25) << 5) | ((inst >>> 7) & 0x1f));
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
				const imm = BigInt(inst >> 20);
				const a = this.getX(rs1);
				const shamt = BigInt((inst >>> 20) & 0x3f);
				let v;
				switch (funct3) {
					case 0: v = a + imm; break; /* addi */
					case 1: v = a << shamt; break;                                /* slli */
					case 2: v = BigInt.asIntN(64, a) < BigInt.asIntN(64, imm) ? 1n : 0n; break; /* slti */
					case 3: v = a < (imm & MASK64) ? 1n : 0n; break; /* sltiu */
					case 4: v = a ^ imm; break; /* xori */
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
				const a32 = Number(this.getX(rs1) & 0xffffffffn) | 0;
				const shamt = (inst >>> 20) & 0x1f;
				let v32;
				switch (funct3) {
					case 0: v32 = (a32 + (inst >> 20)) | 0; break; /* addiw */
					case 1: v32 = a32 << shamt; break;                /* slliw */
					case 5: {
						if (((inst >>> 25) & 0x7f) & 0x20)
							v32 = a32 >> shamt; /* sraiw */
						else
							v32 = (a32 >>> shamt) | 0; /* srliw */
						break;
					}
					default: throw new IllegalInstruction(inst);
				}
				this.setX(rd, BigInt(v32));
				break;
			}
			case 0x33: { /* OP (incl. M extension when funct7=1) */
				const a = this.getX(rs1), b = this.getX(rs2);
				let v;
				if (funct7 === 0x01) {
					switch (funct3) {
						case 0: v = a * b; break;                                             /* mul */
						case 1: v = BigInt.asUintN(64, (BigInt.asIntN(64, a) * BigInt.asIntN(64, b)) >> 64n); break; /* mulh */
						case 2: v = BigInt.asUintN(64, (BigInt.asIntN(64, a) * b) >> 64n); break; /* mulhsu */
						case 3: v = BigInt.asUintN(64, (a * b) >> 64n); break;                        /* mulhu */
						case 4: {
							const as = BigInt.asIntN(64, a), bs = BigInt.asIntN(64, b);
							v = bs === 0n ? MASK64 : (bs === -1n && as === -(1n << 63n) ? as & MASK64 : BigInt.asUintN(64, as / bs));
							break;
						}
						case 5: v = b === 0n ? MASK64 : a / b; break;                                     /* divu */
						case 6: {
							const as = BigInt.asIntN(64, a), bs = BigInt.asIntN(64, b);
							v = bs === 0n ? a : (bs === -1n && as === -(1n << 63n) ? 0n : BigInt.asUintN(64, as % bs));
							break;
						}
						case 7: v = b === 0n ? a : a % b; break;                                            /* remu */
						default: throw new IllegalInstruction(inst);
					}
				} else {
					switch (funct3) {
						case 0: v = funct7 & 0x20 ? a - b : a + b; break; /* sub/add */
						case 1: v = a << (b & 0x3fn); break;             /* sll */
						case 2: v = BigInt.asIntN(64, a) < BigInt.asIntN(64, b) ? 1n : 0n; break; /* slt */
						case 3: v = a < b ? 1n : 0n; break;                                           /* sltu */
						case 4: v = a ^ b; break;                                                       /* xor */
						case 5: v = funct7 & 0x20 ? BigInt.asUintN(64, BigInt.asIntN(64, a) >> (b & 0x3fn)) : a >> (b & 0x3fn); break; /* sra/srl */
						case 6: v = a | b; break;                                                          /* or */
						case 7: v = a & b; break;                                                           /* and */
						default: throw new IllegalInstruction(inst);
					}
				}
				this.setX(rd, v);
				break;
			}
			case 0x3b: { /* OP-32 (incl. M extension when funct7=1) */
				const a32 = Number(this.getX(rs1) & 0xffffffffn) | 0;
				const b32 = Number(this.getX(rs2) & 0xffffffffn) | 0;
				const bshamt = b32 & 0x1f;
				let v32;
				if (funct7 === 0x01) {
					switch (funct3) {
						case 0: v32 = Math.imul(a32, b32); break; /* mulw */
						case 4: v32 = b32 === 0 ? -1 : (b32 === -1 && a32 === -2147483648 ? a32 : Math.trunc(a32 / b32) | 0); break; /* divw */
						case 5: v32 = b32 === 0 ? -1 : (Math.trunc((a32 >>> 0) / (b32 >>> 0)) | 0); break; /* divuw */
						case 6: v32 = b32 === 0 ? a32 : (b32 === -1 && a32 === -2147483648 ? 0 : (a32 % b32) | 0); break; /* remw */
						case 7: v32 = b32 === 0 ? a32 : (((a32 >>> 0) % (b32 >>> 0)) | 0); break; /* remuw */
						default: throw new IllegalInstruction(inst);
					}
				} else {
					switch (funct3) {
						case 0: v32 = funct7 & 0x20 ? (a32 - b32) | 0 : (a32 + b32) | 0; break; /* subw/addw */
						case 1: v32 = a32 << bshamt; break; /* sllw */
						case 5: v32 = funct7 & 0x20 ? a32 >> bshamt : (a32 >>> bshamt) | 0; break; /* sraw/srlw */
						default: throw new IllegalInstruction(inst);
					}
				}
				this.setX(rd, BigInt(v32));
				break;
			}
			case 0x0f: /* MISC-MEM: fence, fence.i -- no-op (single-hart, no cache model) */
				break;
			case 0x07: { /* LOAD-FP: flw/fld -- only these two appear in the histogram */
				const addr = this.xNumber[rs1] + (inst >> 20);
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
				const addr = this.xNumber[rs1] + (((inst >> 25) << 5) | ((inst >>> 7) & 0x1f));
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
				this.raiseTrap(3n, BigInt(pc), false);
				return this.pc;
			}
			if (imm12 === 0x102) { /* sret */
				const sstatus = this.getCsr(CSR.SSTATUS);
				this.priv = (sstatus & SSTATUS_SPP) ? 'S' : 'U';
				let newStatus = (sstatus & SSTATUS_SPIE) ? (sstatus | SSTATUS_SIE) : (sstatus & ~SSTATUS_SIE);
				newStatus |= SSTATUS_SPIE; /* SPIE set to 1 per spec */
				newStatus &= ~SSTATUS_SPP; /* SPP reset to U (least privileged) */
				this.setCsr(CSR.SSTATUS, newStatus);
				return Number(this.getCsr(CSR.SEPC));
			}
			if (imm12 === 0x105) { /* wfi */
				this.halted = true;
				return nextPc;
			}
			if ((inst >>> 25) === 0x09) { /* sfence.vma rs1,rs2 */
				this.flushTlb();
				return nextPc;
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
