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

function loadInitrd(bytes, memory, ramBase) {
    const address = ramBase + 0x04000000;
    new Uint8Array(memory.buffer).set(bytes, address);
}

const kernelPath = process.argv[2] || '../kernel/kernel.elf';
const maxInstructions = Number(process.argv[3] || 200_000_000);
const input = (process.argv[4] || '').replaceAll('\\r', '\r').replaceAll('\\n', '\n');
const expected = (process.argv[5] || 'P5 checkpoint 2 OK').split('\\n');
const initrdPath = process.argv[6];
const wasmPath = new URL('../web/rv64.wasm', import.meta.url);
const [{ instance }, kernel] = await Promise.all([
    WebAssembly.instantiate(await readFile(wasmPath)),
    readFile(kernelPath),
]);
const rv = instance.exports;
const entry = loadElf(kernel, rv.memory, rv.rv_ram());
rv.rv_init(entry);
if (initrdPath) loadInitrd(await readFile(initrdPath), rv.memory, rv.rv_ram());
for (const byte of new TextEncoder().encode(input)) rv.rv_input(byte);

async function serviceFetch() {
    if (rv.rv_fetch_status() !== 1) return;
    const ram = new Uint8Array(rv.memory.buffer);
    const offset = address => rv.rv_ram() + (address >>> 0) - 0x80000000;
    const url = new TextDecoder().decode(ram.subarray(
        offset(rv.rv_fetch_url()), offset(rv.rv_fetch_url()) + rv.rv_fetch_url_length()));
    try {
        const response = await fetch(url);
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const bytes = new Uint8Array(await response.arrayBuffer());
        if (bytes.length > rv.rv_fetch_capacity()) throw new Error('response exceeds guest buffer');
        ram.set(bytes, offset(rv.rv_fetch_destination()));
        rv.rv_fetch_complete(bytes.length, 2);
    } catch {
        rv.rv_fetch_complete(0, 3);
    }
}

let output = '';
let instructions = 0;
const started = performance.now();
while (instructions < maxInstructions && !expected.every(text => output.includes(text))) {
    const batch = Math.min(200_000, maxInstructions - instructions);
    rv.rv_run(batch, 100);
    await serviceFetch();
    instructions += batch;
    while (rv.rv_output_count()) output += String.fromCharCode(rv.rv_output());
}
const seconds = (performance.now() - started) / 1000;
process.stdout.write(output);
console.error(`\n${instructions} instructions in ${seconds.toFixed(2)}s`);
console.error(`pc=0x${rv.rv_pc().toString(16)}`);
if (!expected.every(text => output.includes(text))) process.exitCode = 1;
