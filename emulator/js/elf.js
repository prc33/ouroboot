'use strict';
/* Minimal ELF64 little-endian loader.  File offsets and load addresses in
 * ouroboot are below 4 GiB; read64 still accepts any exactly representable
 * JavaScript Number and rejects larger values. */

var PT_LOAD = 1;

function read16(buf, off) {
	return buf[off] | (buf[off + 1] << 8);
}

function read32(buf, off) {
	return (buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16) |
		(buf[off + 3] << 24)) >>> 0;
}

function read64(buf, off) {
	var lo = read32(buf, off), hi = read32(buf, off + 4);
	if (hi >= 0x200000) throw new Error('ELF64 value exceeds exact JS range');
	return hi * 4294967296 + lo;
}

function loadElf(buf) {
	var machine, entry, phoff, phentsize, phnum, segments, i, off;
	var type, fileOffset, vaddr, filesz, memsz;
	if (buf[0] !== 0x7f || buf[1] !== 0x45 || buf[2] !== 0x4c || buf[3] !== 0x46)
		throw new Error('not an ELF file (bad magic)');
	if (buf[4] !== 2) throw new Error('not ELFCLASS64');
	machine = read16(buf, 18);
	if (machine !== 243) throw new Error('not EM_RISCV');

	entry = read64(buf, 24);
	phoff = read64(buf, 32);
	phentsize = read16(buf, 54);
	phnum = read16(buf, 56);
	segments = [];
	for (i = 0; i < phnum; i++) {
		off = phoff + i * phentsize;
		type = read32(buf, off);
		if (type !== PT_LOAD) continue;
		fileOffset = read64(buf, off + 8);
		vaddr = read64(buf, off + 16);
		filesz = read64(buf, off + 32);
		memsz = read64(buf, off + 40);
		segments.push({ vaddr: vaddr, memsz: memsz,
			data: buf.subarray(fileOffset, fileOffset + filesz) });
	}
	if (segments.length === 0) throw new Error('no PT_LOAD segments');
	return { entry: entry, segments: segments };
}

module.exports = { loadElf: loadElf };
