#!/usr/bin/env node
import { readFile } from 'node:fs/promises';
import { performance } from 'node:perf_hooks';
import { loadElf, loadInitrd, serviceFetch } from '../web/host.js';

const kernelPath = process.argv[2] || '../kernel/kernel.elf';
const maxInstructions = Number(process.argv[3] || 200_000_000);
const input = (process.argv[4] || '').replaceAll('\\r', '\r').replaceAll('\\n', '\n').replaceAll('\\b', '\x7f');
const expected = (process.argv[5] || 'P5 checkpoint 2 OK').split('\\n');
const initrdPath = process.argv[6];
const wasmPath = new URL('../web/rv64.wasm', import.meta.url);
const [{ instance }, kernel] = await Promise.all([
    WebAssembly.instantiate(await readFile(wasmPath)),
    readFile(kernelPath),
]);
const rv = instance.exports;
const entry = loadElf(kernel, rv);
rv.rv_init(entry);
if (initrdPath) loadInitrd(await readFile(initrdPath), rv);
for (const byte of new TextEncoder().encode(input)) rv.rv_input(byte);

let output = '';
let instructions = 0;
const started = performance.now();
while (instructions < maxInstructions && !expected.every(text => output.includes(text))) {
    const batch = Math.min(200_000, maxInstructions - instructions);
    rv.rv_run(batch, 100);
    await serviceFetch(rv).catch(() => {});
    instructions += batch;
    let chunk = '';
    while (rv.rv_output_count()) chunk += String.fromCharCode(rv.rv_output());
    if (chunk) {
        output += chunk;
        process.stdout.write(chunk);
    }
}
const seconds = (performance.now() - started) / 1000;
console.error(`\n${instructions} instructions in ${seconds.toFixed(2)}s`);
console.error(`pc=0x${rv.rv_pc().toString(16)}`);
if (!expected.every(text => output.includes(text))) process.exitCode = 1;
