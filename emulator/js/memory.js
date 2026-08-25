'use strict';
/* Flat physical memory + MMIO dispatch. Sized to cover
 * kernel/arch/riscv64_memmap.h's RV64_RAM_BASE..RV64_MEM_TOP window --
 * this emulator targets that exact kernel, not an arbitrary RISC-V
 * system, so there's no reason to model more address space than the
 * kernel itself ever touches (same "hardcode what we control"
 * reasoning kernel/arch/riscv64_memmap.h itself uses).
 *
 * All addresses are BigInt (RISC-V is a 64-bit ISA; using BigInt
 * throughout rather than splitting into hi/lo 32-bit halves is a
 * correctness-first choice for this milestone -- see
 * docs/emulator-plan.md -- performance work, if ever needed, comes
 * after P1/P2 pass, not before).
 */

const RAM_BASE = 0x80000000n;
const RAM_SIZE = 0x08000000n; /* 128MB, matches kernel/test's `-m 128M` */

class Memory {
	constructor() {
		this.ramBase = RAM_BASE;
		this.ramSize = RAM_SIZE;
		this.ram = new DataView(new ArrayBuffer(Number(RAM_SIZE)));
		this.devices = []; /* [{base, size, read, write}], MMIO ranges checked before RAM */
	}

	addDevice(base, size, dev) {
		this.devices.push({ base, size, dev });
	}

	_findDevice(addr) {
		for (const d of this.devices) {
			if (addr >= d.base && addr < d.base + d.size)
				return d;
		}
		return null;
	}

	_ramOffset(addr) {
		const off = addr - this.ramBase;
		if (off < 0n || off >= this.ramSize)
			return -1;
		return Number(off);
	}

	/* size in bytes: 1, 2, 4, 8. Returns a BigInt (unsigned) always --
	 * callers sign-extend themselves based on the instruction (lb vs
	 * lbu etc), matching how real hardware separates "load this many
	 * bytes" from "then sign- or zero-extend", not conflating the two
	 * here. */
	read(addr, size) {
		const off = this._ramOffset(addr);
		if (off >= 0) {
			switch (size) {
				case 1: return BigInt(this.ram.getUint8(off));
				case 2: return BigInt(this.ram.getUint16(off, true));
				case 4: return BigInt(this.ram.getUint32(off, true));
				case 8: return this.ram.getBigUint64(off, true);
				default: throw new Error(`bad load size ${size}`);
			}
		}

		const dev = this._findDevice(addr);
		if (dev)
			/* Number(), not the raw BigInt offset: device register
			 * offsets are always small, and every device's read/write
			 * switches on the offset with plain Number case labels --
			 * `5n === 5` is always false in JS (BigInt/Number strict
			 * equality never matches even when numerically equal), so
			 * passing the BigInt straight through silently missed
			 * every case and returned 0 for everything. Found by
			 * booting under this emulator: transmit_empty() always
			 * read 0, so serial_putc's poll loop spun forever with no
			 * output and no crash -- looked like a hang, was actually
			 * this. */
			return BigInt(dev.dev.read(Number(addr - dev.base), size)) & mask(size);

		throw new BusError(addr, false);
	}

	write(addr, size, value /* BigInt */) {
		const off = this._ramOffset(addr);
		if (off >= 0) {
			switch (size) {
				case 1: this.ram.setUint8(off, Number(value & 0xffn)); break;
				case 2: this.ram.setUint16(off, Number(value & 0xffffn), true); break;
				case 4: this.ram.setUint32(off, Number(value & 0xffffffffn), true); break;
				case 8: this.ram.setBigUint64(off, value & 0xffffffffffffffffn, true); break;
				default: throw new Error(`bad store size ${size}`);
			}
			return;
		}

		const dev = this._findDevice(addr);
		if (dev) {
			dev.dev.write(Number(addr - dev.base), size, Number(value & mask(size)));
			return;
		}
		throw new BusError(addr, true);
	}

	/* Load raw bytes at a physical address -- used by the ELF loader,
	 * bypassing the byte-at-a-time read/write API for speed. */
	loadBytes(addr, bytes) {
		const off = this._ramOffset(addr);
		if (off < 0 || off + bytes.length > Number(this.ramSize))
			throw new Error(`segment at ${addr.toString(16)} (${bytes.length} bytes) doesn't fit in the modeled RAM window`);
		new Uint8Array(this.ram.buffer, off, bytes.length).set(bytes);
	}
}

function mask(size) {
	switch (size) {
		case 1: return 0xffn;
		case 2: return 0xffffn;
		case 4: return 0xffffffffn;
		case 8: return 0xffffffffffffffffn;
		default: throw new Error(`bad size ${size}`);
	}
}

/* Thrown on access outside both RAM and any registered device -- the
 * CPU catches this and turns it into the appropriate RISC-V access-
 * fault trap (not a page fault -- see mmu.js for the distinction),
 * matching real hardware's behaviour for an address with nothing
 * behind it. */
class BusError extends Error {
	constructor(addr, isWrite) {
		super(`bus error: ${isWrite ? 'write' : 'read'} to unmapped ${addr.toString(16)}`);
		this.addr = addr;
		this.isWrite = isWrite;
	}
}

module.exports = { Memory, BusError, RAM_BASE, RAM_SIZE };
