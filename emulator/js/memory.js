'use strict';
/* Fixed 128 MiB physical RAM and the tiny MMIO bus used by ouroboot.
 * Addresses are unsigned Numbers. The machine maps everything below 4 GiB,
 * so JavaScript represents every physical and virtual address exactly. */

var RAM_BASE = 0x80000000;
var RAM_SIZE = 0x08000000;

function BusError(addr, isWrite) {
	this.name = 'BusError';
	this.message = 'bus error: ' + (isWrite ? 'write' : 'read') +
		' to unmapped ' + (addr >>> 0).toString(16);
	this.addr = addr >>> 0;
	this.isWrite = isWrite;
}
BusError.prototype = Object.create(Error.prototype);
BusError.prototype.constructor = BusError;

function Memory() {
	var buffer = new ArrayBuffer(RAM_SIZE);
	this.ramBase = RAM_BASE;
	this.ramSize = RAM_SIZE;
	this.buffer = buffer;
	this.u8 = new Uint8Array(buffer);
	this.u16 = new Uint16Array(buffer);
	this.u32 = new Uint32Array(buffer);
	this.i32 = new Int32Array(buffer);
	this.devices = [];
}

Memory.prototype.addDevice = function(base, size, dev) {
	this.devices.push({ base: Number(base), size: Number(size), dev: dev });
};

Memory.prototype._findDevice = function(addr) {
	var i, d;
	for (i = 0; i < this.devices.length; i++) {
		d = this.devices[i];
		if (addr >= d.base && addr < d.base + d.size) return d;
	}
	return null;
};

Memory.prototype._ramOffset = function(addr) {
	var off = addr - RAM_BASE;
	return off >= 0 && off < RAM_SIZE ? off : -1;
};

/* Read into valueLo/valueHi, avoiding a pair allocation per access. */
Memory.prototype.readPair = function(addr, size) {
	var off = this._ramOffset(addr), d, lo;
	if (off >= 0 && off + size <= RAM_SIZE) {
		switch (size) {
		case 1: this.valueLo = this.u8[off]; this.valueHi = 0; return;
		case 2: this.valueLo = this.u16[off >> 1]; this.valueHi = 0; return;
		case 4: this.valueLo = this.u32[off >> 2]; this.valueHi = 0; return;
		case 8:
			this.valueLo = this.u32[off >> 2];
			this.valueHi = this.i32[(off >> 2) + 1];
			return;
		default: throw new Error('bad load size ' + size);
		}
	}
	d = this._findDevice(addr);
	if (!d) throw new BusError(addr, false);
	lo = d.dev.read(addr - d.base, size);
	this.valueLo = lo >>> 0;
	this.valueHi = 0;
};

Memory.prototype.writePair = function(addr, size, lo, hi) {
	var off = this._ramOffset(addr), d;
	if (off >= 0 && off + size <= RAM_SIZE) {
		switch (size) {
		case 1: this.u8[off] = lo; return;
		case 2: this.u16[off >> 1] = lo; return;
		case 4: this.u32[off >> 2] = lo; return;
		case 8:
			this.u32[off >> 2] = lo;
			this.i32[(off >> 2) + 1] = hi;
			return;
		default: throw new Error('bad store size ' + size);
		}
	}
	d = this._findDevice(addr);
	if (!d) throw new BusError(addr, true);
	d.dev.write(addr - d.base, size, lo >>> 0);
};

Memory.prototype.loadBytes = function(addr, bytes) {
	var off = this._ramOffset(Number(addr));
	if (off < 0 || off + bytes.length > RAM_SIZE)
		throw new Error('segment does not fit in modeled RAM');
	this.u8.set(bytes, off);
};

module.exports = { Memory: Memory, BusError: BusError,
	RAM_BASE: RAM_BASE, RAM_SIZE: RAM_SIZE };
