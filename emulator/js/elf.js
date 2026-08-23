'use strict';
/* Minimal ELF64 loader -- reads e_entry and every PT_LOAD segment.
 * Independent implementation from mm/elf.c's ELFCLASS64 path (that
 * one runs *inside* the kernel we're trying to emulate, so it can't
 * also be what loads the kernel itself into this emulator's memory);
 * same file format, same PT_LOAD semantics.
 */

const PT_LOAD = 1;

function loadElf(buf) {
	const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);

	if (buf[0] !== 0x7f || buf[1] !== 0x45 || buf[2] !== 0x4c || buf[3] !== 0x46)
		throw new Error('not an ELF file (bad magic)');
	if (buf[4] !== 2)
		throw new Error(`not ELFCLASS64 (e_ident[EI_CLASS]=${buf[4]})`);
	const machine = dv.getUint16(18, true);
	if (machine !== 243)
		throw new Error(`not EM_RISCV (e_machine=${machine})`);

	const e_entry = dv.getBigUint64(24, true);
	const e_phoff = dv.getBigUint64(32, true);
	const e_phentsize = dv.getUint16(54, true);
	const e_phnum = dv.getUint16(56, true);

	const segments = [];
	for (let i = 0; i < e_phnum; i++) {
		const off = Number(e_phoff) + i * e_phentsize;
		const p_type = dv.getUint32(off + 0, true);
		if (p_type !== PT_LOAD)
			continue;
		const p_offset = dv.getBigUint64(off + 8, true);
		const p_vaddr = dv.getBigUint64(off + 16, true);
		const p_filesz = dv.getBigUint64(off + 32, true);
		const p_memsz = dv.getBigUint64(off + 40, true);
		segments.push({
			vaddr: p_vaddr,
			memsz: p_memsz,
			data: buf.subarray(Number(p_offset), Number(p_offset) + Number(p_filesz)),
		});
	}

	if (segments.length === 0)
		throw new Error('no PT_LOAD segments');

	return { entry: e_entry, segments };
}

module.exports = { loadElf };
