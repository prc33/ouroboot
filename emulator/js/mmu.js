'use strict';
/* Sv39 page table walker -- mirrors mm/riscv64_paging.c's own layout
 * exactly (3 levels, 9+9+9+12 bits, same PTE bit meanings), since
 * that's the only page table format kernel/ ever constructs. Not a
 * general Sv39 implementation beyond what that code needs (e.g. no
 * superpage leaf-at-level-1/2 support, since paging_map_page always
 * walks all the way to a level-0 leaf).
 */

const PAGE_SIZE = 4096n;
const PTE_V = 1n;
const PTE_R = 1n << 1n;
const PTE_W = 1n << 2n;
const PTE_X = 1n << 3n;
const PTE_U = 1n << 4n;
const PPN_SHIFT = 10n;

const SATP_MODE_SHIFT = 60n;
const SATP_MODE_SV39 = 8n;
const SATP_PPN_MASK = (1n << 44n) - 1n;

/* access: 'r' | 'w' | 'x'. Returns the physical address, or throws
 * PageFault. mode: current privilege ('S' or 'U'); sum: sstatus.SUM,
 * gating whether S-mode may touch U-only pages (see
 * arch/riscv64_trap.c's trap_init() comment for why the kernel needs
 * this set at all). */
function translate(mem, satp, vaddr, access, mode, sum) {
	const satpMode = satp >> SATP_MODE_SHIFT;
	if (satpMode === 0n)
		return vaddr; /* Bare mode: no translation */
	if (satpMode !== SATP_MODE_SV39)
		throw new Error(`unsupported satp MODE ${satpMode} (only Bare and Sv39 implemented)`);

	let tableAddr = (satp & SATP_PPN_MASK) << 12n;
	let pte = 0n;
	for (let level = 2; level >= 0; level--) {
		const shift = 12n + 9n * BigInt(level);
		const idx = (vaddr >> shift) & 0x1ffn;
		const pteAddr = tableAddr + idx * 8n;
		pte = mem.read(pteAddr, 8);
		if (!(pte & PTE_V))
			throw new PageFault(vaddr, access);
		const isLeaf = (pte & (PTE_R | PTE_W | PTE_X)) !== 0n;
		if (!isLeaf) {
			tableAddr = ((pte >> PPN_SHIFT) << 12n);
			continue;
		}
		/* leaf -- permission check */
		if (access === 'r' && !(pte & PTE_R)) throw new PageFault(vaddr, access);
		if (access === 'w' && !(pte & PTE_W)) throw new PageFault(vaddr, access);
		if (access === 'x' && !(pte & PTE_X)) throw new PageFault(vaddr, access);
		if (pte & PTE_U) {
			if (mode === 'U') {
				/* fine */
			} else if (mode === 'S' && !sum) {
				throw new PageFault(vaddr, access);
			}
		} else if (mode === 'U') {
			throw new PageFault(vaddr, access); /* U-mode touching a non-U page */
		}
		const ppn = pte >> PPN_SHIFT;
		return (ppn << 12n) | (vaddr & 0xfffn);
	}
	throw new PageFault(vaddr, access); /* walked to level -1 without a leaf */
}

class PageFault extends Error {
	constructor(vaddr, access) {
		super(`page fault: ${access} at ${vaddr.toString(16)}`);
		this.vaddr = vaddr;
		this.access = access;
	}
}

module.exports = { translate, PageFault, PAGE_SIZE };
