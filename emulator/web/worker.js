'use strict';
import { loadElf, loadInitrd, serviceFetch as fetchGuest } from './host.js';

let rv;
let running = false;
let fetching = false;

async function serviceFetch() {
    if (fetching || rv.rv_fetch_status() !== 1) return;
    fetching = true;
    try {
        await fetchGuest(rv);
    } catch (error) {
        rv.rv_fetch_complete(0, 3);
        postMessage({ type: 'output', bytes: [...new TextEncoder().encode(`fetch: ${error.message}\r\n`)] });
    } finally {
        fetching = false;
    }
}

function flushOutput() {
    const bytes = [];
    while (rv.rv_output_count()) bytes.push(rv.rv_output());
    if (bytes.length) postMessage({ type: 'output', bytes });
}

function runChunk() {
    if (!running) return;
    try {
        rv.rv_run(2000000, 100);
        flushOutput();
        serviceFetch();
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
    const entry = loadElf(new Uint8Array(elfBuffer), rv);
    loadInitrd(new Uint8Array(initrdBuffer), rv);
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
