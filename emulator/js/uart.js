'use strict';
/* 16550 UART, matching drivers/riscv64_serial.c's register expectations
 * exactly (same offsets it writes during serial_init(), same transmit-
 * empty polling protocol serial_putc() uses) -- MMIO at 0x10000000,
 * same as QEMU's riscv64 `virt` machine (see docs/riscv-port-findings.md
 * for how that address was confirmed originally).
 *
 * Minimal on purpose: this emulator only needs to make the kernel's
 * own polling driver work, not model a real 16550's full feature set
 * (FIFOs, real baud-rate timing, RX interrupts -- the kernel doesn't
 * use RX at all yet). Transmit-empty (LSR bit 5) is hardwired always-
 * ready, since there's no real transmit latency to model.
 */

var REG_THR = 0; /* write: transmit holding register (also RBR/DLL on read/DLAB) */
var REG_IER = 1;
var REG_FCR = 2; /* also IIR on read */
var REG_LCR = 3;
var REG_MCR = 4;
var REG_LSR = 5;

var LSR_THRE = 0x20; /* transmit holding register empty */
var LSR_DR = 0x01;   /* data ready (RX) */

function Uart16550(onByte) {
		this.onByte = onByte || function() {}; /* called with each transmitted byte */
		this.rxQueue = [];
}

	/* Feed input bytes in (e.g. from a browser terminal, or stdin in
	 * Node) -- exercised since checkpoint 10 by kernel/Makefile's own
	 * test-js target (RISCV64_INTERACTIVE_INPUT, fed via boot.js's
	 * --input) and by the browser demo's real keyboard input
	 * (app.js's term.onData -> worker.js -> here), now that
	 * kernel/arch/riscv64_syscall.c's sys_read has a real reader on
	 * the other end. */
Uart16550.prototype.pushInput = function(byte) {
		this.rxQueue.push(byte & 0xff);
};

Uart16550.prototype.read = function(offset, size) {
		if (size !== 1)
			throw new Error('UART: unexpected ' + size + '-byte read at offset ' + offset);
		switch (offset) {
			case REG_THR: /* RBR when DLAB=0 */
				return this.rxQueue.length ? this.rxQueue.shift() : 0;
			case REG_LSR:
				return LSR_THRE | (this.rxQueue.length ? LSR_DR : 0);
			default:
				return 0;
		}
};

Uart16550.prototype.write = function(offset, size, value) {
		if (size !== 1)
			throw new Error('UART: unexpected ' + size + '-byte write at offset ' + offset);
		if (offset === REG_THR)
			this.onByte(value & 0xff);
		/* IER/FCR/LCR/MCR: accepted, not modeled -- serial_init()'s
		 * baud-rate/framing setup has no observable effect here since
		 * there's no real wire to configure. */
	};

module.exports = { Uart16550: Uart16550, UART_BASE: 0x10000000 };
