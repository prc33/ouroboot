'use strict';
/* Web Worker running the CPU loop off the main thread, so it never
 * blocks the terminal UI (docs/emulator-plan.md's P3). Loads the same
 * untouched core files boot.js and kernel/Makefile's test-js target
 * already exercise, via browser-shim.js's loadCjsModule() -- each
 * file gets its own isolated function scope, same as Node's own
 * require() (see browser-shim.js's header comment for the bug this
 * replaced: naive importScripts() shares one global scope across
 * every file, and a module-private `const CSR = ...` in one file can
 * collide with another's). No bundler, no build step (E2).
 *
 * Protocol (postMessage, all JSON-safe -- no BigInt across the
 * worker boundary):
 *   main -> worker: { type: 'boot', elfBytes: ArrayBuffer }
 *   main -> worker: { type: 'input', byte: number }        (future: keyboard -> UART RX)
 *   worker -> main: { type: 'output', bytes: number[] }    (UART TX bytes, batched)
 *   worker -> main: { type: 'halted' | 'error', message?: string }
 */

importScripts('browser-shim.js');

const { Memory } = loadCjsModule('memory');
const { Uart16550, UART_BASE } = loadCjsModule('uart');
const { Cpu } = loadCjsModule('cpu');
const { loadElf } = loadCjsModule('elf');

let cpu = null;
let uart = null;
let running = false;

/* Batches output bytes and flushes on a timer rather than one
 * postMessage per byte -- printing a whole kernel boot banner
 * character-by-character across worker->main messages would be
 * needless overhead for no benefit (xterm.js is equally happy with a
 * batch). */
let outBuf = [];
function flushOutput() {
	if (outBuf.length) {
		postMessage({ type: 'output', bytes: outBuf });
		outBuf = [];
	}
}
setInterval(flushOutput, 16); /* ~60fps, plenty for a text terminal */

function runChunk() {
	if (!running) return;
	/* Run a bounded chunk per tick rather than a tight infinite loop,
	 * so the worker's own event loop gets a chance to process incoming
	 * postMessages (e.g. a future stop/reset command) between chunks --
	 * same reasoning as any cooperative-yielding loop. */
	const CHUNK = 200000;
	try {
		for (let i = 0; i < CHUNK; i++) {
			cpu.step(100n); /* time-advance=100, matches test-js's tuning -- see kernel/Makefile */
		}
	} catch (e) {
		running = false;
		flushOutput();
		postMessage({ type: 'error', message: e.message + '\n' + (e.stack || '') });
		return;
	}
	flushOutput();
	setTimeout(runChunk, 0);
}

onmessage = function (ev) {
	const msg = ev.data;
	if (msg.type === 'boot') {
		const mem = new Memory();
		uart = new Uart16550((byte) => outBuf.push(byte));
		mem.addDevice(UART_BASE, 8n, uart);

		const elf = loadElf(new Uint8Array(msg.elfBytes));
		for (const seg of elf.segments)
			mem.loadBytes(seg.vaddr, seg.data);

		cpu = new Cpu(mem);
		cpu.pc = elf.entry;
		running = true;
		runChunk();
	} else if (msg.type === 'input') {
		if (uart) uart.pushInput(msg.byte);
	} else if (msg.type === 'stop') {
		running = false;
	}
};
