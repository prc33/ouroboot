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

var csrDefs = require('./csr');
var CSR = csrDefs.CSR;
var SSTATUS_SIE = csrDefs.SSTATUS_SIE;
var SSTATUS_SPIE = csrDefs.SSTATUS_SPIE;
var SSTATUS_SPP = csrDefs.SSTATUS_SPP;
var SSTATUS_SUM = csrDefs.SSTATUS_SUM;
var SIP_STIP = csrDefs.SIP_STIP;
var mmuDefs = require('./mmu');
var translate = mmuDefs.translate;
var PageFault = mmuDefs.PageFault;
var memoryDefs = require('./memory');
var BusError = memoryDefs.BusError;
var RAM_BASE = memoryDefs.RAM_BASE;

function IllegalInstruction(inst) {
	this.name = 'IllegalInstruction';
	this.message = 'illegal instruction ' + (inst >>> 0).toString(16);
}
IllegalInstruction.prototype = Object.create(Error.prototype);
IllegalInstruction.prototype.constructor = IllegalInstruction;

/* Scratch typed-array views for float64<->bits and float32<->bits
 * reinterpretation (JS has no direct "punning" operator). Module-
 * level and reused across calls since it's write-then-immediately-read,
 * never held across a yield point. */
var fConvBuf = new ArrayBuffer(8);
var fConvU32 = new Uint32Array(fConvBuf);
var fConvF64 = new Float64Array(fConvBuf);
var fConvF32 = new Float32Array(fConvBuf);

function Cpu(mem) {
		var i;
		this.mem = mem;
		/* RV64 values as two unboxed 32-bit halves.  The low half is unsigned;
		 * the high half is signed so comparisons and arithmetic shifts map
		 * directly to JavaScript's native 32-bit operators. */
		this.xLo = new Uint32Array(32);
		this.xHi = new Int32Array(32);
		/* 32 float/double registers, stored as paired raw 32-bit halves --
		 * the single source of truth, same bit representation as real
		 * hardware. Single-precision values are NaN-boxed (upper 32 bits
		 * all 1s) per the D-extension spec; getF32/setF32 below implement
		 * that and getF64/setF64 implement the plain double case. */
		this.fLo = new Uint32Array(32);
		this.fHi = new Int32Array(32);
		/* Every executable mapping in this machine is below 4GB, where a JS
		 * Number is exact. Full-width GPR arithmetic uses the paired arrays. */
		this.pc = 0;
		this.priv = 1; /* 0=U, 1=S; no M-mode */
		this.csrLo = new Uint32Array(4096);
		this.csrHi = new Int32Array(4096);
		/* Hot CSR mirrors avoid typed-array lookups during fetch and interrupt
		 * checks; setCsrPair keeps both representations together. */
		this.satpLo = 0;
		this.satpHi = 0;
		this.sstatus = 0;
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
		this.tlbVpage = -1;
		this.tlbRamOffset = -1; /* the fetch fast path returns straight from RAM, so the physical
		                          * page itself is never needed again once this offset is cached */
		this.tlbSatpLo = 0;
		this.tlbSatpHi = 0;
		this.tlbContext = -1;
		/* Loads and stores range across many pages, so each gets a tiny
		 * direct-mapped 256-page cache. The indexing rule is the replacement
		 * policy; there is no list, allocation, or search on the hot path. */
		this.dataTlbVpage = new Float64Array(512);
		for (i = 0; i < 512; i++) this.dataTlbVpage[i] = -1;
		/* All physical addresses in this deliberately fixed machine are below
		 * 4 GiB, so cached physical pages are exact Numbers. */
		this.dataTlbPpage = new Float64Array(512);
		this.dataTlbSatpLo = new Uint32Array(512);
		this.dataTlbSatpHi = new Int32Array(512);
		this.dataTlbContext = new Int8Array(512);
		/* Convolution coefficients exceed 32 bits before carry propagation. */
		this.mulDigits = new Float64Array(8);
}

Cpu.prototype.setPair = function(i, lo, hi) {
		if (i !== 0) {
			this.xLo[i] = lo >>> 0;
			this.xHi[i] = hi | 0;
		}
};
Cpu.prototype.setSigned32 = function(i, lo) { this.setPair(i, lo, lo >> 31); }
Cpu.prototype.umul32High = function(a, b) {
		var a0 = a & 0xffff, a1 = a >>> 16;
		var b0 = b & 0xffff, b1 = b >>> 16;
		var w0 = Math.imul(a0, b0);
		var t = (Math.imul(a1, b0) >>> 0) + (w0 >>> 16);
		var w1 = (t & 0xffff) + (Math.imul(a0, b1) >>> 0);
		return (Math.imul(a1, b1) + Math.floor(t / 65536) + Math.floor(w1 / 65536)) >>> 0;
};
Cpu.prototype.multiplyLow = function(alo, ahi, blo, bhi) {
		this.valueLo = Math.imul(alo, blo) >>> 0;
		this.valueHi = (this.umul32High(alo, blo) +
			Math.imul(alo, bhi) + Math.imul(ahi, blo)) | 0;
}

Cpu.prototype.negatePair = function(lo, hi) {
		var nlo = (-lo) >>> 0;
		this.valueLo = nlo;
		this.valueHi = (~hi + (nlo === 0 ? 1 : 0)) | 0;
};

	/* Full 64x64 product in eight base-2^16 digits.  This is used only by
	 * M-extension instructions; common adds, shifts and addresses never
	 * enter a helper. */
Cpu.prototype.multiplyPair = function(alo, ahi, blo, bhi, signedA, signedB, highResult) {
		var neg = false, i, j, carry, k;
		var d = this.mulDigits;
		if (signedA && ahi < 0) {
			this.negatePair(alo, ahi); alo = this.valueLo; ahi = this.valueHi; neg = !neg;
		}
		if (signedB && bhi < 0) {
			this.negatePair(blo, bhi); blo = this.valueLo; bhi = this.valueHi; neg = !neg;
		}
		var a = [alo & 0xffff, alo >>> 16, ahi & 0xffff, (ahi >>> 16) & 0xffff];
		var b = [blo & 0xffff, blo >>> 16, bhi & 0xffff, (bhi >>> 16) & 0xffff];
		for (i = 0; i < 8; i++) d[i] = 0;
		for (i = 0; i < 4; i++)
			for (j = 0; j < 4; j++) d[i + j] += a[i] * b[j];
		carry = 0;
		for (i = 0; i < 8; i++) {
			carry += d[i]; d[i] = carry & 0xffff; carry = Math.floor(carry / 65536);
		}
		if (neg) {
			carry = 1;
			for (i = 0; i < 8; i++) { k = ((~d[i]) & 0xffff) + carry; d[i] = k & 0xffff; carry = k >>> 16; }
		}
		i = highResult ? 4 : 0;
		this.valueLo = (d[i] | (d[i + 1] << 16)) >>> 0;
		this.valueHi = (d[i + 2] | (d[i + 3] << 16)) | 0;
}

Cpu.prototype.dividePair = function(alo, ahi, blo, bhi, signed, remainder) {
		var negQ = false, negR = false, qlo = 0, qhi = 0, rlo = 0, rhi = 0;
		var i, bit, nlo, nhi, av, bv, result;
		if ((blo | bhi) === 0) {
			this.valueLo = remainder ? alo : 0xffffffff;
			this.valueHi = remainder ? ahi : -1;
			return;
		}
		/* C library divisions overwhelmingly operate on sign-extended 32-bit
		 * values.  Keep that path in native Number arithmetic; the restoring
		 * divider below is only for genuinely wide operands. */
		if (signed && ahi === (alo >> 31) && bhi === (blo >> 31)) {
			av = alo | 0; bv = blo | 0;
			result = remainder ? av % bv : Math.trunc(av / bv);
			this.valueLo = result >>> 0; this.valueHi = result >> 31;
			return;
		}
		if (!signed && ahi === 0 && bhi === 0) {
			result = remainder ? alo % blo : Math.floor(alo / blo);
			this.valueLo = result >>> 0; this.valueHi = 0;
			return;
		}
		if (signed && ahi < 0) { this.negatePair(alo, ahi); alo = this.valueLo; ahi = this.valueHi; negQ = true; negR = true; }
		if (signed && bhi < 0) { this.negatePair(blo, bhi); blo = this.valueLo; bhi = this.valueHi; negQ = !negQ; }
		for (i = 63; i >= 0; i--) {
			bit = i >= 32 ? (ahi >>> (i - 32)) & 1 : (alo >>> i) & 1;
			rhi = ((rhi << 1) | (rlo >>> 31)) | 0; rlo = ((rlo << 1) | bit) >>> 0;
			if ((rhi >>> 0) > (bhi >>> 0) || (rhi === bhi && rlo >= blo)) {
				nlo = (rlo - blo) >>> 0; nhi = (rhi - bhi - (rlo < blo ? 1 : 0)) | 0;
				rlo = nlo; rhi = nhi;
				if (i >= 32) qhi |= 1 << (i - 32); else qlo |= 1 << i;
			}
		}
		this.valueLo = remainder ? rlo : qlo;
		this.valueHi = remainder ? rhi : qhi;
		if ((remainder ? negR : negQ)) this.negatePair(this.valueLo, this.valueHi);
}

Cpu.prototype.getF64 = function(i) {
		fConvU32[0] = this.fLo[i];
		fConvU32[1] = this.fHi[i];
		return fConvF64[0];
};
Cpu.prototype.setF64 = function(i, val) {
		fConvF64[0] = val;
		this.fLo[i] = fConvU32[0];
		this.fHi[i] = fConvU32[1];
};

	/* NaN-boxing (D-ext spec 12.2): a single-precision value in a 64-bit
	 * F register is only valid if the upper 32 bits are all 1s; anything
	 * else reads back as the canonical 32-bit quiet NaN. Not expected to
	 * ever trigger here (nothing in the histogram writes f-regs any way
	 * but setF32/setF64 above), but it's part of the spec's actual
	 * semantics, not an edge case worth skipping. */
Cpu.prototype.getF32 = function(i) {
		if (this.fHi[i] !== -1) return NaN;
		fConvU32[0] = this.fLo[i];
		return fConvF32[0];
};
Cpu.prototype.setF32 = function(i, val) {
		fConvF32[0] = val;
		this.fLo[i] = fConvU32[0];
		this.fHi[i] = -1;
};

	/* fsgnj.s/fsgnj.d: rs1's magnitude, rs2's sign bit -- operates on raw
	 * bits, not the numeric value (so it's correct for NaNs too, unlike
	 * doing this via Math.abs()/multiply). fmv.s/fmv.d (rs2===rs1, a
	 * self-copy) fall out of this for free, matching how the real ISA
	 * defines them as pseudo-instructions rather than separate opcodes. */
Cpu.prototype.rs2SignOnF32 = function(rs1, rs2) {
		fConvU32[0] = (this.fLo[rs1] & 0x7fffffff) |
			(this.fLo[rs2] & 0x80000000);
		return fConvF32[0];
};
Cpu.prototype.rs2SignOnF64 = function(rs1, rs2) {
		fConvU32[0] = this.fLo[rs1];
		fConvU32[1] = (this.fHi[rs1] & 0x7fffffff) |
			(this.fHi[rs2] & 0x80000000);
		return fConvF64[0];
}

Cpu.prototype.getCsrPair = function(addr) {
		if (addr === CSR.TIME) {
			this.valueLo = this.time >>> 0;
			this.valueHi = Math.floor(this.time / 4294967296) | 0;
			return;
		}
		this.valueLo = this.csrLo[addr];
		this.valueHi = this.csrHi[addr];
};
Cpu.prototype.setCsrPair = function(addr, lo, hi) {
		this.csrLo[addr] = lo >>> 0;
		this.csrHi[addr] = hi | 0;
		if (addr === CSR.SATP) {
			this.satpLo = lo >>> 0;
			this.satpHi = hi | 0;
		}
		if (addr === CSR.SSTATUS) {
			this.sstatus = lo >>> 0;
			this.sum = (lo & SSTATUS_SUM) !== 0;
		}
		if (addr === CSR.STIMECMP) {
			this.timecmp = (hi >>> 0) * 4294967296 + (lo >>> 0);
			if (this.timecmp === 0 || this.time < this.timecmp)
				this.csrLo[CSR.SIP] &= ~SIP_STIP;
		}
};

	/* --- memory access, MMU + fault translation --- */

Cpu.prototype.translateAddr = function(vaddr, access) {
		var satpLo = this.satpLo;
		var satpHi = this.satpHi;
		var sum = this.sum;
		var vpage = Math.floor(vaddr / 4096);
		var context = (this.priv === 0 ? 1 : 0) | (sum ? 2 : 0);
		var slot = (vpage & 0xff) + (access === 'w' ? 256 : 0);
		if (this.dataTlbVpage[slot] === vpage &&
		    this.dataTlbSatpLo[slot] === satpLo &&
		    this.dataTlbSatpHi[slot] === satpHi &&
		    this.dataTlbContext[slot] === context)
			return this.dataTlbPpage[slot] + (vaddr & 0xfff);

		var paddr = translate(this.mem, satpLo, satpHi, vaddr, access, this.priv, sum);
		this.dataTlbVpage[slot] = vpage;
		this.dataTlbPpage[slot] = Math.floor(paddr / 4096) * 4096;
		this.dataTlbSatpLo[slot] = satpLo;
		this.dataTlbSatpHi[slot] = satpHi;
		this.dataTlbContext[slot] = context;
		return paddr;
}

Cpu.prototype.flushTlb = function() {
		var i;
		this.tlbVpage = -1;
		for (i = 0; i < 512; i++) this.dataTlbVpage[i] = -1;
}

Cpu.prototype.fetch32 = function(vaddr) {
		var satpLo = this.satpLo;
		var satpHi = this.satpHi;
		var sum = this.sum;
		var vpage = Math.floor(vaddr / 4096);
		var context = (this.priv === 0 ? 1 : 0) | (sum ? 2 : 0);
		if (this.tlbVpage === vpage &&
		    this.tlbSatpLo === satpLo &&
		    this.tlbSatpHi === satpHi &&
		    this.tlbContext === context &&
		    this.tlbRamOffset >= 0)
			return this.mem.u32[(this.tlbRamOffset + (vaddr & 0xfff)) >> 2];

		var paddr = translate(this.mem, satpLo, satpHi, vaddr, 'x', this.priv, sum);
		this.tlbVpage = vpage;
		this.tlbSatpLo = satpLo;
		this.tlbSatpHi = satpHi;
		this.tlbContext = context;
		var off = paddr - RAM_BASE;
		if (off < 0 || off + 4 > this.mem.ramSize) {
			this.tlbRamOffset = -1;
			this.mem.readPair(paddr, 4);
			return this.mem.valueLo;
		}
		this.tlbRamOffset = Math.floor(paddr / 4096) * 4096 - RAM_BASE;
		return this.mem.u32[off >> 2];
};

	/* Numeric paired-word variant used by the instruction hot path. */
Cpu.prototype.loadPair = function(vaddr, size, signed) {
		var paddr = this.translateAddr(vaddr, 'r');
		var off = paddr - 0x80000000;
		var lo;
		if (off >= 0 && off + size <= 0x08000000) {
			switch (size) {
				case 1: lo = this.mem.u8[off]; break;
				case 2: lo = this.mem.u16[off >> 1]; break;
				case 4: lo = this.mem.u32[off >> 2]; break;
				case 8:
					this.valueLo = this.mem.u32[off >> 2];
					this.valueHi = this.mem.i32[(off >> 2) + 1];
					return;
				default: throw new Error('bad load size ' + size);
			}
		} else {
			this.mem.readPair(paddr, size);
			lo = this.mem.valueLo;
		}
		this.valueLo = lo >>> 0;
		this.valueHi = signed ? (lo << (32 - size * 8)) >> 31 : 0;
}

Cpu.prototype.storePair = function(vaddr, size, lo, hi) {
		var paddr = this.translateAddr(vaddr, 'w');
		var off = paddr - 0x80000000;
		if (off >= 0 && off + size <= 0x08000000) {
			switch (size) {
				case 1: this.mem.u8[off] = lo; return;
				case 2: this.mem.u16[off >> 1] = lo; return;
				case 4: this.mem.u32[off >> 2] = lo; return;
				case 8:
					this.mem.u32[off >> 2] = lo;
					this.mem.i32[(off >> 2) + 1] = hi;
					return;
			}
		}
		this.mem.writePair(paddr, size, lo, hi);
};

	/* --- traps --- */

Cpu.prototype.raiseTrap = function(cause, tvalLo, tvalHi, isInterrupt) {
		var sstatus = this.sstatus;
		this.setCsrPair(CSR.SEPC, this.pc, 0);
		this.setCsrPair(CSR.SCAUSE, cause, isInterrupt ? -2147483648 : 0);
		this.setCsrPair(CSR.STVAL, tvalLo, tvalHi);

		sstatus = this.priv === 1 ? (sstatus | SSTATUS_SPP) : (sstatus & ~SSTATUS_SPP);
		var sie = (sstatus & SSTATUS_SIE) ? 1 : 0;
		sstatus = (sstatus & ~SSTATUS_SPIE) | (sie << 5);
		sstatus &= ~SSTATUS_SIE;
		this.setCsrPair(CSR.SSTATUS, sstatus, 0);

		this.priv = 1;
		this.pc = (this.csrLo[CSR.STVEC] & ~3) >>> 0;
		this.halted = false;
};

	/* Maps a caught access exception to the right scause and re-raises
	 * as a trap. `access` is 'x'/'r'/'w', matching mmu.js's own
	 * vocabulary, so the three access-fault/page-fault cause codes
	 * (which differ only in a fixed offset per access type) fall out
	 * of one table instead of three near-duplicate catch blocks. */
Cpu.prototype.handleAccessFault = function(e, access) {
		var causes = {
			x: { bus: 1, page: 12 },
			r: { bus: 5, page: 13 },
			w: { bus: 7, page: 15 },
		}[access];
		if (e instanceof PageFault) {
			this.raiseTrap(causes.page, e.vaddr, 0, false);
			return true;
		}
		if (e instanceof BusError) {
			this.raiseTrap(causes.bus, e.addr, 0, false);
			return true;
		}
		return false;
}

Cpu.prototype.checkInterrupts = function() {
		if (this.timecmp === 0 || this.time < this.timecmp)
			return false;
		this.csrLo[CSR.SIP] |= SIP_STIP;

		var sstatus = this.sstatus;
		var globallyEnabled = this.priv === 0 || (this.priv === 1 && (sstatus & SSTATUS_SIE));
		var pending = this.csrLo[CSR.SIP] & this.csrLo[CSR.SIE] & SIP_STIP;
		if (globallyEnabled && pending) {
			this.halted = false;
			this.raiseTrap(5, 0, 0, true); /* supervisor timer interrupt */
			return true;
		}
		return false;
};

	/* --- main loop --- */

Cpu.prototype.step = function(timeAdvance) {
		this.run(1, timeAdvance);
};

	/* Run a host-side batch without paying one JS method call per guest
	 * instruction. Interrupts and time still advance instruction by
	 * instruction inside the loop. */
Cpu.prototype.run = function(count, timeAdvance) {
		for (var i = 0; i < count; i++) {
			/* WFI does not retire instructions while the hart waits.  The old
			 * loop nevertheless visited every synthetic timer tick, which made an
			 * idle guest as expensive as a busy one.  Jump directly to the next
			 * timer deadline; the interrupt check below still observes precisely
			 * the same architectural time. */
			if (this.halted && this.timecmp !== 0 && this.time < this.timecmp) {
				var skip = Math.min(count - i - 1,
					Math.max(0, Math.ceil((this.timecmp - this.time) / timeAdvance) - 1));
				this.time += skip * timeAdvance;
				i += skip;
			}
			this.time += timeAdvance;
			if (this.timecmp !== 0 && this.time >= this.timecmp && this.checkInterrupts())
				continue;
			if (this.halted)
				continue;

			var inst;
			try {
				inst = this.fetch32(this.pc);
			} catch (fetchError) {
				if (this.handleAccessFault(fetchError, 'x')) continue;
				throw fetchError;
			}

			try {
				this.execute(inst);
			} catch (executeError) {
				if (executeError instanceof IllegalInstruction) {
					this.raiseTrap(2, inst, 0, false);
					continue;
				}
				if (this.handleAccessFault(executeError, executeError.accessKind || 'r')) continue;
				throw executeError;
			}
		}
};

	/* --- decode + execute --- */

Cpu.prototype.execute = function(inst) {
		var opcode = inst & 0x7f;
		var rd = (inst >>> 7) & 0x1f;
		var funct3 = (inst >>> 12) & 0x7;
		var rs1 = (inst >>> 15) & 0x1f;
		var rs2 = (inst >>> 20) & 0x1f;
		var funct7 = (inst >>> 25) & 0x7f;

		var pc = this.pc;
		var nextPc = pc + 4;

		switch (opcode) {
			case 0x37: { /* LUI */
				var lo = (inst & 0xfffff000) | 0;
				this.setPair(rd, lo, lo >> 31);
				break;
			}
			case 0x17: { /* AUIPC */
				var imm = (inst & 0xfffff000) | 0;
				var lo = (pc + imm) >>> 0;
				var hi = ((imm >> 31) + (lo < (pc >>> 0) ? 1 : 0)) | 0;
				this.setPair(rd, lo, hi);
				break;
			}
			case 0x6f: { /* JAL */
				var imm = (((inst >>> 21) & 0x3ff) << 1) |
				          (((inst >>> 20) & 0x1) << 11) |
				          (((inst >>> 12) & 0xff) << 12) |
				          (((inst >>> 31) & 0x1) << 20);
				if (imm & 0x100000) imm -= 0x200000;
				this.setPair(rd, nextPc, 0);
				nextPc = pc + imm;
				break;
			}
			case 0x67: { /* JALR */
				var imm = inst >> 20;
				var target = (this.xLo[rs1] + imm) >>> 0;
				this.setPair(rd, nextPc, 0);
				nextPc = (target & ~1) >>> 0;
				break;
			}
			case 0x63: { /* BRANCH */
				var imm = (((inst >>> 8) & 0xf) << 1) |
				          (((inst >>> 25) & 0x3f) << 5) |
				          (((inst >>> 7) & 0x1) << 11) |
				          (((inst >>> 31) & 0x1) << 12);
				if (imm & 0x1000) imm -= 0x2000;
				var alo = this.xLo[rs1], blo = this.xLo[rs2];
				var ahi = this.xHi[rs1], bhi = this.xHi[rs2];
				var taken;
				switch (funct3) {
					case 0: taken = alo === blo && ahi === bhi; break; /* beq */
					case 1: taken = alo !== blo || ahi !== bhi; break; /* bne */
					case 4: taken = ahi < bhi || (ahi === bhi && alo < blo); break; /* blt */
					case 5: taken = ahi > bhi || (ahi === bhi && alo >= blo); break; /* bge */
					case 6: taken = (ahi >>> 0) < (bhi >>> 0) || (ahi === bhi && alo < blo); break; /* bltu */
					case 7: taken = (ahi >>> 0) > (bhi >>> 0) || (ahi === bhi && alo >= blo); break; /* bgeu */
					default: throw new IllegalInstruction(inst);
				}
				if (taken) nextPc = pc + imm;
				break;
			}
			case 0x03: { /* LOAD */
				var addr = this.xLo[rs1] + (inst >> 20);
				try {
					switch (funct3) {
						case 0: this.loadPair(addr, 1, true); break;  /* lb */
						case 1: this.loadPair(addr, 2, true); break;  /* lh */
						case 2: this.loadPair(addr, 4, true); break;  /* lw */
						case 3: this.loadPair(addr, 8, false); break; /* ld */
						case 4: this.loadPair(addr, 1, false); break; /* lbu */
						case 5: this.loadPair(addr, 2, false); break; /* lhu */
						case 6: this.loadPair(addr, 4, false); break; /* lwu */
						default: throw new IllegalInstruction(inst);
					}
				} catch (loadError) { loadError.accessKind = 'r'; throw loadError; }
				this.setPair(rd, this.valueLo, this.valueHi);
				break;
			}
			case 0x23: { /* STORE */
				var addr = this.xLo[rs1] + (((inst >> 25) << 5) | ((inst >>> 7) & 0x1f));
				try {
					switch (funct3) {
						case 0: this.storePair(addr, 1, this.xLo[rs2], this.xHi[rs2]); break; /* sb */
						case 1: this.storePair(addr, 2, this.xLo[rs2], this.xHi[rs2]); break; /* sh */
						case 2: this.storePair(addr, 4, this.xLo[rs2], this.xHi[rs2]); break; /* sw */
						case 3: this.storePair(addr, 8, this.xLo[rs2], this.xHi[rs2]); break; /* sd */
						default: throw new IllegalInstruction(inst);
					}
				} catch (storeError) { storeError.accessKind = 'w'; throw storeError; }
				break;
			}
			case 0x13: { /* OP-IMM */
				var imm = inst >> 20;
				var alo = this.xLo[rs1], ahi = this.xHi[rs1];
				var shamt = (inst >>> 20) & 0x3f;
				var lo = 0, hi = 0;
				switch (funct3) {
					case 0: /* addi */
						lo = (alo + imm) >>> 0;
						hi = (ahi + (imm >> 31) + (lo < alo ? 1 : 0)) | 0;
						break;
					case 1: /* slli */
						if (shamt === 0) { lo = alo; hi = ahi; }
						else if (shamt < 32) { lo = alo << shamt; hi = (ahi << shamt) | (alo >>> (32 - shamt)); }
						else { lo = 0; hi = alo << (shamt - 32); }
						break;
					case 2: lo = (ahi < (imm >> 31) || (ahi === (imm >> 31) && alo < (imm >>> 0))) ? 1 : 0; break; /* slti */
					case 3: lo = ((ahi >>> 0) < ((imm >> 31) >>> 0) || (ahi === (imm >> 31) && alo < (imm >>> 0))) ? 1 : 0; break; /* sltiu */
					case 4: lo = alo ^ imm; hi = ahi ^ (imm >> 31); break; /* xori */
					case 5: {
						var arithmetic = ((inst >>> 25) & 0x20) !== 0;
						if (shamt === 0) { lo = alo; hi = ahi; }
						else if (shamt < 32) {
							lo = (alo >>> shamt) | (ahi << (32 - shamt));
							hi = arithmetic ? ahi >> shamt : ahi >>> shamt;
						} else {
							lo = arithmetic ? ahi >> (shamt - 32) : ahi >>> (shamt - 32);
							hi = arithmetic ? ahi >> 31 : 0;
						}
						break;
					}
					case 6: lo = alo | imm; hi = ahi | (imm >> 31); break; /* ori */
					case 7: lo = alo & imm; hi = ahi & (imm >> 31); break; /* andi */
					default: throw new IllegalInstruction(inst);
				}
				this.setPair(rd, lo, hi);
				break;
			}
			case 0x1b: { /* OP-IMM-32 */
				var a32 = this.xLo[rs1] | 0;
				var shamt = (inst >>> 20) & 0x1f;
				var v32;
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
				this.setSigned32(rd, v32);
				break;
			}
			case 0x33: { /* OP (incl. M extension when funct7=1) */
				if (funct7 === 0x01) {
					var alo = this.xLo[rs1], ahi = this.xHi[rs1];
					var blo = this.xLo[rs2], bhi = this.xHi[rs2];
					switch (funct3) {
						case 0: this.multiplyLow(alo, ahi, blo, bhi); break;
						case 1: this.multiplyPair(alo, ahi, blo, bhi, true, true, true); break;
						case 2: this.multiplyPair(alo, ahi, blo, bhi, true, false, true); break;
						case 3: this.multiplyPair(alo, ahi, blo, bhi, false, false, true); break;
						case 4: this.dividePair(alo, ahi, blo, bhi, true, false); break;
						case 5: this.dividePair(alo, ahi, blo, bhi, false, false); break;
						case 6: this.dividePair(alo, ahi, blo, bhi, true, true); break;
						case 7: this.dividePair(alo, ahi, blo, bhi, false, true); break;
						default: throw new IllegalInstruction(inst);
					}
					this.setPair(rd, this.valueLo, this.valueHi);
				} else {
					var alo = this.xLo[rs1], blo = this.xLo[rs2];
					var ahi = this.xHi[rs1], bhi = this.xHi[rs2];
					var shamt = blo & 63;
					var lo = 0, hi = 0;
					switch (funct3) {
						case 0:
							if (funct7 & 0x20) { lo = (alo - blo) >>> 0; hi = (ahi - bhi - (alo < blo ? 1 : 0)) | 0; }
							else { lo = (alo + blo) >>> 0; hi = (ahi + bhi + (lo < alo ? 1 : 0)) | 0; }
							break;
						case 1:
							if (shamt === 0) { lo = alo; hi = ahi; }
							else if (shamt < 32) { lo = alo << shamt; hi = (ahi << shamt) | (alo >>> (32 - shamt)); }
							else { hi = alo << (shamt - 32); }
							break;
						case 2: lo = ahi < bhi || (ahi === bhi && alo < blo) ? 1 : 0; break;
						case 3: lo = (ahi >>> 0) < (bhi >>> 0) || (ahi === bhi && alo < blo) ? 1 : 0; break;
						case 4: lo = alo ^ blo; hi = ahi ^ bhi; break;
						case 5:
							if (shamt === 0) { lo = alo; hi = ahi; }
							else if (shamt < 32) { lo = (alo >>> shamt) | (ahi << (32 - shamt)); hi = funct7 & 0x20 ? ahi >> shamt : ahi >>> shamt; }
							else { lo = funct7 & 0x20 ? ahi >> (shamt - 32) : ahi >>> (shamt - 32); hi = funct7 & 0x20 ? ahi >> 31 : 0; }
							break;
						case 6: lo = alo | blo; hi = ahi | bhi; break;
						case 7: lo = alo & blo; hi = ahi & bhi; break;
						default: throw new IllegalInstruction(inst);
					}
					this.setPair(rd, lo, hi);
				}
				break;
			}
			case 0x3b: { /* OP-32 (incl. M extension when funct7=1) */
				var a32 = this.xLo[rs1] | 0;
				var b32 = this.xLo[rs2] | 0;
				var bshamt = b32 & 0x1f;
				var v32;
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
				this.setSigned32(rd, v32);
				break;
			}
			case 0x0f: /* MISC-MEM: fence, fence.i -- no-op (single-hart, no cache model) */
				break;
			case 0x07: { /* LOAD-FP: flw/fld -- only these two appear in the histogram */
				var addr = this.xLo[rs1] + (inst >> 20);
				try {
					switch (funct3) {
						case 2: /* flw -- raw 32-bit load, NaN-boxed into the 64-bit f-reg */
							this.loadPair(addr, 4, false);
							this.fLo[rd] = this.valueLo; this.fHi[rd] = -1;
							break;
						case 3: /* fld -- raw 64-bit load */
							this.loadPair(addr, 8, false);
							this.fLo[rd] = this.valueLo; this.fHi[rd] = this.valueHi;
							break;
						default: throw new IllegalInstruction(inst);
					}
				} catch (floatLoadError) { floatLoadError.accessKind = 'r'; throw floatLoadError; }
				break;
			}
			case 0x27: { /* STORE-FP: fsw/fsd */
				var addr = this.xLo[rs1] + (((inst >> 25) << 5) | ((inst >>> 7) & 0x1f));
				try {
					switch (funct3) {
						case 2: this.storePair(addr, 4, this.fLo[rs2], this.fHi[rs2]); break; /* fsw */
						case 3: this.storePair(addr, 8, this.fLo[rs2], this.fHi[rs2]); break; /* fsd */
						default: throw new IllegalInstruction(inst);
					}
				} catch (floatStoreError) { floatStoreError.accessKind = 'w'; throw floatStoreError; }
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
						this.setF32(rd, Math.fround(this.xLo[rs1] | 0));
						break;
					case 0x71: /* FMV.X.D / FCLASS.D: only funct3=0 (fmv.x.d, raw bit move) needed */
						if (rs2 !== 0 || funct3 !== 0) throw new IllegalInstruction(inst);
						this.setPair(rd, this.fLo[rs1], this.fHi[rs1]);
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

Cpu.prototype.executeSystem = function(inst, rd, funct3, rs1, rs2, funct7, pc, nextPc) {
		if (funct3 === 0) {
			var imm12 = inst >>> 20;
			if (imm12 === 0x000) { /* ecall */
				var cause = this.priv === 0 ? 8 : 9;
				this.raiseTrap(cause, 0, 0, false);
				return this.pc; /* raiseTrap already set pc to stvec */
			}
			if (imm12 === 0x001) { /* ebreak */
				this.raiseTrap(3, pc, 0, false);
				return this.pc;
			}
			if (imm12 === 0x102) { /* sret */
				var sstatus = this.sstatus;
				this.priv = (sstatus & SSTATUS_SPP) ? 1 : 0;
				var newStatus = (sstatus & SSTATUS_SPIE) ? (sstatus | SSTATUS_SIE) : (sstatus & ~SSTATUS_SIE);
				newStatus |= SSTATUS_SPIE; /* SPIE set to 1 per spec */
				newStatus &= ~SSTATUS_SPP; /* SPP reset to U (least privileged) */
				this.setCsrPair(CSR.SSTATUS, newStatus, 0);
				return this.csrLo[CSR.SEPC];
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
		var csrAddr = (inst >>> 20) & 0xfff;
		this.getCsrPair(csrAddr);
		var oldLo = this.valueLo, oldHi = this.valueHi;
		var useImm = funct3 >= 5;
		var srcLo = useImm ? rs1 : this.xLo[rs1];
		var srcHi = useImm ? 0 : this.xHi[rs1];
		var updatedLo, updatedHi;
		switch (funct3 & 0x3) {
			case 1: updatedLo = srcLo; updatedHi = srcHi; break;
			case 2: updatedLo = oldLo | srcLo; updatedHi = oldHi | srcHi; break;
			case 3: updatedLo = oldLo & ~srcLo; updatedHi = oldHi & ~srcHi; break;
			default: throw new IllegalInstruction(inst);
		}
		/* rs1==0 (or zimm==0) for the read-modify-write forms means
		 * "don't write" per spec -- doesn't apply to csrrw(i), which
		 * always writes. */
		var isWriteForm = (funct3 & 0x3) === 1;
		if (isWriteForm || rs1 !== 0)
			this.setCsrPair(csrAddr, updatedLo, updatedHi);
		this.setPair(rd, oldLo, oldHi);
		return nextPc;
};

module.exports = { Cpu: Cpu, IllegalInstruction: IllegalInstruction };
