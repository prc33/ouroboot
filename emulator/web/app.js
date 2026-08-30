'use strict';
const term = new Terminal({
	cursorBlink: true,
	convertEol: true,
	fontFamily: 'monospace',
	theme: { background: '#000000', foreground: '#e0e0e0' },
});
term.open(document.getElementById('terminal'));
term.write('Fetching kernel and initrd...\r\n');
window.term = term;
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
term.onData((data) => {
	for (let i = 0; i < data.length; i++)
		worker.postMessage({ type: 'input', byte: data.charCodeAt(i) });
});

const params = new URLSearchParams(location.search);
const kernelPath = params.get('kernel') || '../../kernel/kernel.elf';
const initrdPath = params.get('initrd') || '../../kernel/tcc-initrd.tar';

Promise.all([kernelPath, initrdPath].map((path) => fetch(path).then((r) => {
	if (!r.ok) throw new Error(`fetching ${path}: HTTP ${r.status}`);
	return r.arrayBuffer();
}))).then(([elfBytes, initrdBytes]) => {
	term.write(`Booting ${kernelPath} with ${initrdPath}...\r\n\r\n`);
	worker.postMessage({ type: 'boot', elfBytes, initrdBytes }, [elfBytes, initrdBytes]);
})
	.catch((e) => {
		term.write(`\r\n\x1b[31mfailed to fetch boot files: ${e.message}\x1b[0m\r\n`);
	});
