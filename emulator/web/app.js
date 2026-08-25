'use strict';
/* Main-thread glue between xterm.js and the C/Wasm emulator worker. */

const term = new Terminal({
	cursorBlink: true,
	convertEol: true,
	fontFamily: 'monospace',
	theme: { background: '#000000', foreground: '#e0e0e0' },
});
term.open(document.getElementById('terminal'));
term.write('Fetching kernel.elf...\r\n');
window.term = term; /* debugging/testability hook, e.g. reading term.buffer from outside */

const worker = new Worker('worker.js');
window.emulatorWorker = worker;

worker.onmessage = function (ev) {
	const msg = ev.data;
	if (msg.type === 'output') {
		term.write(Uint8Array.from(msg.bytes));
	} else if (msg.type === 'error') {
		term.write(`\r\n\x1b[31m[emulator error] ${msg.message}\x1b[0m\r\n`);
	} else if (msg.type === 'halted') {
		term.write('\r\n[cpu halted]\r\n');
	}
};

/* Keyboard input becomes bytes in the C emulator's UART RX ring. */
term.onData((data) => {
	for (let i = 0; i < data.length; i++)
		worker.postMessage({ type: 'input', byte: data.charCodeAt(i) });
});

const kernelPath = new URLSearchParams(location.search).get('kernel') || '../../kernel/kernel.elf';
fetch(kernelPath)
	.then((r) => {
		if (!r.ok) throw new Error(`fetching ${kernelPath}: HTTP ${r.status}`);
		return r.arrayBuffer();
	})
	.then((elfBytes) => {
		term.write(`Booting ${kernelPath} (${elfBytes.byteLength} bytes)...\r\n\r\n`);
		worker.postMessage({ type: 'boot', elfBytes }, [elfBytes]);
	})
	.catch((e) => {
		term.write(`\r\n\x1b[31mfailed to fetch kernel: ${e.message}\x1b[0m\r\n`);
	});
