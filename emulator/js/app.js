'use strict';
/* Main-thread glue: xterm.js terminal <-> Worker running the CPU
 * (worker.js). No CPU emulation code runs here at all -- everything
 * in this file is I/O plumbing (docs/emulator-plan.md's P3). */

var term = new Terminal({
	cursorBlink: true,
	convertEol: true,
	fontFamily: 'monospace',
	theme: { background: '#000000', foreground: '#e0e0e0' },
});
term.open(document.getElementById('terminal'));
term.write('Fetching kernel.elf...\r\n');
window.term = term; /* debugging/testability hook, e.g. reading term.buffer from outside */

var worker = new Worker('worker.js');
window.emulatorWorker = worker;

worker.onmessage = function (ev) {
	var msg = ev.data;
	if (msg.type === 'output') {
		term.write(new Uint8Array(msg.bytes));
	} else if (msg.type === 'error') {
		term.write('\r\n\x1b[31m[emulator error] ' + msg.message + '\x1b[0m\r\n');
	} else if (msg.type === 'halted') {
		term.write('\r\n[cpu halted]\r\n');
	}
};

/* Keyboard input -> UART RX. Wired up since P3 for a future that
 * checkpoint 10 (kernel/arch/riscv64_syscall.c's sys_read) has now
 * arrived at: a real interactive busybox ash reading real stdin.
 * Verified end-to-end via Puppeteer -- typed keystrokes reaching this
 * handler, going out over worker.postMessage, landing in the kernel's
 * own UART RX FIFO (uart.js's pushInput()), and coming back out the
 * other side as real shell output. */
term.onData(function(data) {
	for (var i = 0; i < data.length; i++)
		worker.postMessage({ type: 'input', byte: data.charCodeAt(i) });
});

var kernelPath = new URLSearchParams(location.search).get('kernel') || '../../kernel/kernel.elf';
fetch(kernelPath)
	.then(function(r) {
		if (!r.ok) throw new Error('fetching ' + kernelPath + ': HTTP ' + r.status);
		return r.arrayBuffer();
	})
	.then(function(elfBytes) {
		term.write('Booting ' + kernelPath + ' (' + elfBytes.byteLength + ' bytes)...\r\n\r\n');
		worker.postMessage({ type: 'boot', elfBytes: elfBytes }, [elfBytes]);
	})
	.catch(function(fetchError) {
		term.write('\r\n\x1b[31mfailed to fetch kernel: ' + fetchError.message + '\x1b[0m\r\n');
	});
