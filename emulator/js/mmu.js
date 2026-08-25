'use strict';
/* Sv39 walker specialized for ouroboot's below-4-GiB mappings.  satp is
 * supplied as low/high 32-bit words; the resulting physical address is an
 * exact JavaScript Number. */

var PAGE_SIZE = 4096;
var PTE_V = 1, PTE_R = 2, PTE_W = 4, PTE_X = 8, PTE_U = 16;

function PageFault(vaddr, access) {
	this.name = 'PageFault';
	this.message = 'page fault: ' + access + ' at ' +
		(vaddr >>> 0).toString(16);
	this.vaddr = vaddr >>> 0;
	this.access = access;
}
PageFault.prototype = Object.create(Error.prototype);
PageFault.prototype.constructor = PageFault;

function translate(mem, satpLo, satpHi, vaddr, access, priv, sum) {
	var mode = satpHi >>> 28;
	var tableAddr, level, shift, idx, pteAddr, pteLo, leaf;
	if (mode === 0) return vaddr >>> 0;
	if (mode !== 8) throw new Error('unsupported satp mode ' + mode);

	/* All page tables allocated by this kernel are below 4 GiB, so the
	 * physical page number needed here is wholly in satpLo. */
	tableAddr = (satpLo * 4096) >>> 0;
	for (level = 2; level >= 0; level--) {
		shift = 12 + 9 * level;
		idx = Math.floor(vaddr / Math.pow(2, shift)) & 0x1ff;
		pteAddr = tableAddr + idx * 8;
		mem.readPair(pteAddr, 8);
		pteLo = mem.valueLo;
		if (!(pteLo & PTE_V)) throw new PageFault(vaddr, access);
		leaf = (pteLo & (PTE_R | PTE_W | PTE_X)) !== 0;
		if (!leaf) {
			tableAddr = ((pteLo >>> 10) * 4096) >>> 0;
			continue;
		}
		if (access === 'r' && !(pteLo & PTE_R)) throw new PageFault(vaddr, access);
		if (access === 'w' && !(pteLo & PTE_W)) throw new PageFault(vaddr, access);
		if (access === 'x' && !(pteLo & PTE_X)) throw new PageFault(vaddr, access);
		if (pteLo & PTE_U) {
			if (priv !== 0 && !sum) throw new PageFault(vaddr, access);
		} else if (priv === 0) {
			throw new PageFault(vaddr, access);
		}
		return (((pteLo >>> 10) * 4096) + (vaddr & 0xfff)) >>> 0;
	}
	throw new PageFault(vaddr, access);
}

module.exports = { translate: translate, PageFault: PageFault,
	PAGE_SIZE: PAGE_SIZE };
