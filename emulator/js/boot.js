#!/usr/bin/env node
'use strict';
/* Headless boot harness -- P1's exit criterion from docs/emulator-plan.md:
 * boot a riscv64 kernel.elf under this emulator and check the captured
 * UART output against the same assertion strings
 * kernel/test/boot_test.py already uses under QEMU. Node first (fast
 * iteration, no browser needed) -- see docs/emulator-plan.md.
 *
 * Usage: node boot.js <kernel.elf> [--max-instructions N]
 *                      [--must-contain STR ...] [--must-not-contain STR ...]
 *                      [--input STR]
 *
 * --input: bytes queued into the UART's RX side (uart.js's own
 * pushInput(), there since P3 but never exercised until checkpoint 10
 * gave the kernel a real blocking stdin reader) before boot starts.
 * All queued at once rather than paced against instruction count or
 * wall time -- sys_read's own blocking loop (arch/riscv64_syscall.c)
 * only ever consumes one byte per read() call regardless of how many
 * are already queued, so this is equivalent to "the user typed
 * everything instantly", not a timing simulation; correct for an
 * automated correctness check, not a demo of realistic typing speed. */
const fs = require('fs');
const { Memory, RAM_BASE } = require('./memory');
const { Uart16550, UART_BASE } = require('./uart');
const { Cpu } = require('./cpu');
const { loadElf } = require('./elf');

function parseArgs(argv) {
	const args = { mustContain: [], mustNotContain: [], maxInstructions: 200_000_000, timeAdvance: 1, input: '' };
	let kernel = null;
	for (let i = 0; i < argv.length; i++) {
		const a = argv[i];
		if (a === '--must-contain') args.mustContain.push(argv[++i]);
		else if (a === '--must-not-contain') args.mustNotContain.push(argv[++i]);
		else if (a === '--max-instructions') args.maxInstructions = Number(argv[++i]);
		else if (a === '--time-advance') args.timeAdvance = Number(argv[++i]);
		else if (a === '--input') args.input = argv[++i].replace(/\\n/g, '\n').replace(/\\r/g, '\r'); /* literal \n/\r -> real newline/CR, same convention as test/boot_test.py's --stdin-input */
		else if (!kernel) kernel = a;
		else throw new Error(`unexpected arg: ${a}`);
	}
	if (!kernel) throw new Error('usage: boot.js <kernel.elf> [--must-contain STR]... [--must-not-contain STR]...');
	args.kernel = kernel;
	return args;
}

function boot(args) {
	const mem = new Memory();
	let output = '';
	const uart = new Uart16550((byte) => { output += String.fromCharCode(byte); });
	mem.addDevice(UART_BASE, 8n, uart);
	for (let i = 0; i < args.input.length; i++)
		uart.pushInput(args.input.charCodeAt(i));

	const buf = fs.readFileSync(args.kernel);
	const elf = loadElf(new Uint8Array(buf));
	for (const seg of elf.segments)
		mem.loadBytes(seg.vaddr, seg.data);

	const cpu = new Cpu(mem);
	cpu.pc = Number(elf.entry);
	/* Real hardware/QEMU leaves sp garbage at reset -- boot/riscv64_boot.S
	 * is responsible for setting it itself (see kernel/arch/riscv64_memmap.h).
	 * We deliberately do NOT set x2 here, so a bug in that assumption
	 * would surface the same way it would on real hardware. */

	let n = 0;
	let checkedOutputLength = -1;
	const batchSize = 10_000;
	for (; n < args.maxInstructions;) {
		const batch = Math.min(batchSize, args.maxInstructions - n);
		cpu.run(batch, args.timeAdvance);
		n += batch;
		/* Required strings can only become true when the UART emits output.
		 * BusyBox executes millions of instructions between characters, so
		 * rescanning the whole transcript after every instruction was pure
		 * harness overhead. */
		if (output.length !== checkedOutputLength) {
			checkedOutputLength = output.length;
			if (args.mustContain.every((s) => output.includes(s)))
				break; /* early exit once everything we're waiting for has shown up */
		}
	}

	return { output, instructionsRun: n, cpu };
}

function main() {
	const args = parseArgs(process.argv.slice(2));
	console.error(`--- booting ${args.kernel} under the JS emulator (max ${args.maxInstructions} instructions) ---`);
	const { output, instructionsRun } = boot(args);

	console.error('--- captured UART output ---');
	console.error(output || '(empty)');
	console.error('--- end UART output ---');
	console.error(`(${instructionsRun} instructions executed)`);

	const failures = [];
	if (!output.trim())
		failures.push('no UART output at all');
	for (const needle of args.mustContain)
		if (!output.includes(needle))
			failures.push(`expected string not found: ${JSON.stringify(needle)}`);
	for (const needle of args.mustNotContain)
		if (output.includes(needle))
			failures.push(`forbidden string found: ${JSON.stringify(needle)}`);

	if (failures.length) {
		console.error(`\nFAIL (${failures.length} assertion(s) failed):`);
		for (const f of failures) console.error(`  - ${f}`);
		process.exit(1);
	}
	console.error(`\nPASS (${args.mustContain.length} required string(s) found, ${args.mustNotContain.length} forbidden string(s) absent)`);
}

if (require.main === module)
	main();

module.exports = { boot, parseArgs };
