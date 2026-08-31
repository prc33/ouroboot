export function loadElf(bytes, rv) {
    if (bytes.length < 64) throw new Error('truncated ELF header');
    const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (v.getUint32(0, false) !== 0x7f454c46 || bytes[4] !== 2 || bytes[5] !== 1 ||
        v.getUint16(18, true) !== 243) throw new Error('expected a 64-bit little-endian RISC-V ELF file');
    const entry = v.getBigUint64(24, true);
    const phoff = Number(v.getBigUint64(32, true));
    const phsize = v.getUint16(54, true), phnum = v.getUint16(56, true);
    if (phsize !== 56 || phoff > bytes.length || phnum > (bytes.length - phoff) / phsize)
        throw new Error('invalid program headers');
    const memory = new Uint8Array(rv.memory.buffer), base = BigInt(0x80000000);
    let entryOk = false;
    for (let i = 0; i < phnum; i++) {
        const p = phoff + i * phsize;
        if (v.getUint32(p, true) !== 1) continue;
        const offset = Number(v.getBigUint64(p + 8, true));
        const guest = v.getBigUint64(p + 16, true);
        const filesz = Number(v.getBigUint64(p + 32, true));
        const memsz = Number(v.getBigUint64(p + 40, true));
        if (guest < base || guest - base > BigInt(rv.rv_ram_size()) || filesz > memsz ||
            memsz > rv.rv_ram_size() - Number(guest - base) || offset > bytes.length ||
            filesz > bytes.length - offset) throw new Error('invalid load segment');
        const address = rv.rv_ram() + Number(guest - base);
        memory.set(bytes.subarray(offset, offset + filesz), address);
        memory.fill(0, address + filesz, address + memsz);
        if ((v.getUint32(p + 4, true) & 1) && entry >= guest && entry - guest < BigInt(memsz)) entryOk = true;
    }
    if (!entryOk) throw new Error('entry is not in an executable load segment');
    return entry;
}

export function loadInitrd(bytes, rv) {
    if (bytes.byteLength > 0x01000000) throw new Error('initrd exceeds the kernel 16 MiB limit');
    new Uint8Array(rv.memory.buffer).set(bytes, rv.rv_ram() + 0x04000000);
}

export async function serviceFetch(rv) {
    if (rv.rv_fetch_status() !== 1) return;
    const ram = new Uint8Array(rv.memory.buffer);
    const offset = address => rv.rv_ram() + (address >>> 0) - 0x80000000;
    try {
        const start = offset(rv.rv_fetch_url());
        const url = new TextDecoder().decode(ram.subarray(start, start + rv.rv_fetch_url_length()));
        const response = await fetch(url);
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const bytes = new Uint8Array(await response.arrayBuffer());
        if (bytes.length > rv.rv_fetch_capacity()) throw new Error('response exceeds guest buffer');
        ram.set(bytes, offset(rv.rv_fetch_destination()));
        rv.rv_fetch_complete(bytes.length, 2);
    } catch (error) {
        rv.rv_fetch_complete(0, 3);
        throw error;
    }
}
