#!/usr/bin/env node
import { readFile } from 'node:fs/promises';
import { performance } from 'node:perf_hooks';

function loadElf(bytes, memory, ramBase) {
    const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (v.getUint32(0, false) !== 0x7f454c46 || bytes[4] !== 2 || v.getUint16(18, true) !== 243)
        throw new Error('expected a 64-bit RISC-V ELF file');
    const entry = v.getBigUint64(24, true);
    const phoff = Number(v.getBigUint64(32, true));
    const phsize = v.getUint16(54, true);
    const phnum = v.getUint16(56, true);
    const ram = new Uint8Array(memory.buffer);
    for (let i = 0; i < phnum; i++) {
        const p = phoff + i * phsize;
        if (v.getUint32(p, true) !== 1) continue;
        const fileOffset = Number(v.getBigUint64(p + 8, true));
        const address = Number(v.getBigUint64(p + 16, true) - 0x80000000n) + ramBase;
        const fileSize = Number(v.getBigUint64(p + 32, true));
        ram.set(bytes.subarray(fileOffset, fileOffset + fileSize), address);
    }
    return entry;
}

const kernelPath = process.argv[2] || '../kernel/kernel.elf';
const maxInstructions = Number(process.argv[3] || 200_000_000);
const input = (process.argv[4] || '').replaceAll('\\r', '\r').replaceAll('\\n', '\n');
const expected = process.argv[5] || 'P5 checkpoint 2 OK';
const wasmPath = new URL('../web/rv64.wasm', import.meta.url);
const [{ instance }, kernel] = await Promise.all([
    WebAssembly.instantiate(await readFile(wasmPath)),
    readFile(kernelPath),
]);
const rv = instance.exports;
const entry = loadElf(kernel, rv.memory, rv.rv_ram());
rv.rv_init(entry);
for (const byte of new TextEncoder().encode(input)) rv.rv_input(byte);

let output = '';
let instructions = 0;
const started = performance.now();
while (instructions < maxInstructions && !output.includes(expected)) {
    const batch = Math.min(200_000, maxInstructions - instructions);
    rv.rv_run(batch, 100);
    instructions += batch;
    while (rv.rv_output_count()) output += String.fromCharCode(rv.rv_output());
}
const seconds = (performance.now() - started) / 1000;
process.stdout.write(output);
console.error(`\n${instructions} instructions in ${seconds.toFixed(2)}s`);
console.error(`pc=0x${rv.rv_pc().toString(16)}`);
if (!output.includes(expected)) process.exitCode = 1;
