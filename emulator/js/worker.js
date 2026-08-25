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

var memoryDefs = loadCjsModule('memory');
var Memory = memoryDefs.Memory;
var uartDefs = loadCjsModule('uart');
var Uart16550 = uartDefs.Uart16550;
var UART_BASE = uartDefs.UART_BASE;
var Cpu = loadCjsModule('cpu').Cpu;
var loadElf = loadCjsModule('elf').loadElf;

var cpu = null;
var uart = null;
var running = false;

/* Batches output bytes and flushes on a timer rather than one
 * postMessage per byte -- printing a whole kernel boot banner
 * character-by-character across worker->main messages would be
 * needless overhead for no benefit (xterm.js is equally happy with a
 * batch). */
var outBuf = [];
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
	var CHUNK = 200000;
	try {
		cpu.run(CHUNK, 100); /* time-advance=100, matches test-js's tuning -- see kernel/Makefile */
	} catch (runError) {
		running = false;
		flushOutput();
		postMessage({ type: 'error', message: runError.message + '\n' + (runError.stack || '') });
		return;
	}
	flushOutput();
	setTimeout(runChunk, 0);
}

onmessage = function (ev) {
	var msg = ev.data;
	if (msg.type === 'boot') {
		var mem = new Memory(), elf, i, seg;
		uart = new Uart16550(function(byte) { outBuf.push(byte); });
		mem.addDevice(UART_BASE, 8, uart);

		elf = loadElf(new Uint8Array(msg.elfBytes));
		for (i = 0; i < elf.segments.length; i++) {
			seg = elf.segments[i];
			mem.loadBytes(seg.vaddr, seg.data);
		}

		cpu = new Cpu(mem);
		cpu.pc = Number(elf.entry);
		running = true;
		runChunk();
	} else if (msg.type === 'input') {
		if (uart) uart.pushInput(msg.byte);
	} else if (msg.type === 'stop') {
		running = false;
	}
};
