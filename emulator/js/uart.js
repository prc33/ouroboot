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

const REG_THR = 0; /* write: transmit holding register (also RBR/DLL on read/DLAB) */
const REG_IER = 1;
const REG_FCR = 2; /* also IIR on read */
const REG_LCR = 3;
const REG_MCR = 4;
const REG_LSR = 5;

const LSR_THRE = 0x20; /* transmit holding register empty */
const LSR_DR = 0x01;   /* data ready (RX) */

class Uart16550 {
	constructor(onByte) {
		this.onByte = onByte || (() => {}); /* called with each transmitted byte */
		this.rxQueue = [];
	}

	/* Feed input bytes in (e.g. from a browser terminal, or stdin in
	 * Node) -- not exercised by kernel/'s own test suite (it never
	 * reads), but implemented since a real terminal needs it, and it
	 * costs nothing to have ready for P3. */
	pushInput(byte) {
		this.rxQueue.push(byte & 0xff);
	}

	read(offset, size) {
		if (size !== 1)
			throw new Error(`UART: unexpected ${size}-byte read at offset ${offset}`);
		switch (offset) {
			case REG_THR: /* RBR when DLAB=0 */
				return this.rxQueue.length ? this.rxQueue.shift() : 0;
			case REG_LSR:
				return LSR_THRE | (this.rxQueue.length ? LSR_DR : 0);
			default:
				return 0;
		}
	}

	write(offset, size, value) {
		if (size !== 1)
			throw new Error(`UART: unexpected ${size}-byte write at offset ${offset}`);
		if (offset === REG_THR)
			this.onByte(value & 0xff);
		/* IER/FCR/LCR/MCR: accepted, not modeled -- serial_init()'s
		 * baud-rate/framing setup has no observable effect here since
		 * there's no real wire to configure. */
	}
}

module.exports = { Uart16550, UART_BASE: 0x10000000n };
