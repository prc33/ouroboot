'use strict';

let rv;
let running = false;

function loadElf(bytes) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (view.getUint32(0, false) !== 0x7f454c46 || bytes[4] !== 2 ||
        view.getUint16(18, true) !== 243)
        throw new Error('expected a 64-bit RISC-V ELF file');
    const entry = view.getBigUint64(24, true);
    const phoff = Number(view.getBigUint64(32, true));
    const phsize = view.getUint16(54, true);
    const phnum = view.getUint16(56, true);
    const memory = new Uint8Array(rv.memory.buffer);
    const ram = rv.rv_ram();
    for (let i = 0; i < phnum; i++) {
        const p = phoff + i * phsize;
        if (view.getUint32(p, true) !== 1) continue;
        const offset = Number(view.getBigUint64(p + 8, true));
        const address = ram + Number(view.getBigUint64(p + 16, true) - 0x80000000n);
        const size = Number(view.getBigUint64(p + 32, true));
        memory.set(bytes.subarray(offset, offset + size), address);
    }
    return entry;
}

function loadInitrd(bytes) {
    const start = rv.rv_ram() + 0x04000000;
    if (bytes.byteLength > 0x01000000)
        throw new Error('initrd exceeds the kernel 16 MiB limit');
    new Uint8Array(rv.memory.buffer).set(bytes, start);
}

function flushOutput() {
    const bytes = [];
    while (rv.rv_output_count()) bytes.push(rv.rv_output());
    if (bytes.length) postMessage({ type: 'output', bytes });
}

function runChunk() {
    if (!running) return;
    try {
        rv.rv_run(200000, 100);
        flushOutput();
        setTimeout(runChunk, 0);
    } catch (error) {
        running = false;
        postMessage({ type: 'error', message: error.stack || error.message });
    }
}

async function boot(elfBuffer, initrdBuffer) {
    const response = await fetch('rv64.wasm');
    if (!response.ok) throw new Error(`fetching rv64.wasm: HTTP ${response.status}`);
    ({ instance: { exports: rv } } = await WebAssembly.instantiate(await response.arrayBuffer()));
    const entry = loadElf(new Uint8Array(elfBuffer));
    loadInitrd(new Uint8Array(initrdBuffer));
    rv.rv_init(entry);
    running = true;
    runChunk();
}

onmessage = ({ data }) => {
    if (data.type === 'boot') {
        boot(data.elfBytes, data.initrdBytes).catch(error =>
            postMessage({ type: 'error', message: error.stack || error.message }));
    } else if (data.type === 'input' && rv) {
        rv.rv_input(data.byte);
    } else if (data.type === 'stop') {
        running = false;
    }
};
