'use strict';
/* Headless boot harness -- P1's exit criterion from docs/emulator-plan.md:
 * boot a riscv64 kernel.elf under this emulator and check the captured
 * UART output against the same assertion strings
 * kernel/test/boot_test.py already uses under QEMU. Node first (fast
 * iteration, no browser needed) -- see docs/emulator-plan.md.
 *
 * Usage: node boot.js <kernel.elf> [--max-instructions N] [--max-seconds N]
 *                      [--must-contain STR ...] [--must-not-contain STR ...]
 *                      [--input STR]
 *
 * --max-seconds: a speed *regression* test, not a correctness one --
 * fails the run if the CPU loop alone (not module load, not ELF
 * parsing) takes longer than this many wall-clock seconds. Exists
 * because "still produces the right output" and "still fast" are two
 * different properties the interpreter can lose independently (see
 * this repo's own git history for a real case: the TLB-less
 * interpreter was already correct, just slow enough that checkpoint
 * 10's interactive read() loop took minutes instead of seconds).
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
var fs = require('fs');
var memoryDefs = require('./memory');
var Memory = memoryDefs.Memory;
var uartDefs = require('./uart');
var Uart16550 = uartDefs.Uart16550;
var UART_BASE = uartDefs.UART_BASE;
var Cpu = require('./cpu').Cpu;
var loadElf = require('./elf').loadElf;

function parseArgs(argv) {
	var args = { mustContain: [], mustNotContain: [], maxInstructions: 200000000, maxSeconds: null, timeAdvance: 1, input: '' };
	var kernel = null, i, a;
	for (i = 0; i < argv.length; i++) {
		a = argv[i];
		if (a === '--must-contain') args.mustContain.push(argv[++i]);
		else if (a === '--must-not-contain') args.mustNotContain.push(argv[++i]);
		else if (a === '--max-instructions') args.maxInstructions = Number(argv[++i]);
		else if (a === '--max-seconds') args.maxSeconds = Number(argv[++i]);
		else if (a === '--time-advance') args.timeAdvance = Number(argv[++i]);
		else if (a === '--input') args.input = argv[++i].replace(/\\n/g, '\n').replace(/\\r/g, '\r'); /* literal \n/\r -> real newline/CR, same convention as test/boot_test.py's --stdin-input */
		else if (!kernel) kernel = a;
		else throw new Error('unexpected arg: ' + a);
	}
	if (!kernel) throw new Error('usage: boot.js <kernel.elf> [--must-contain STR]... [--must-not-contain STR]...');
	args.kernel = kernel;
	return args;
}

function boot(args) {
	var mem = new Memory();
	var output = '';
	var uart = new Uart16550(function(byte) { output += String.fromCharCode(byte); });
	mem.addDevice(UART_BASE, 8, uart);
	var i, seg, n = 0, checkedOutputLength = -1;
	for (i = 0; i < args.input.length; i++)
		uart.pushInput(args.input.charCodeAt(i));

	var buf = fs.readFileSync(args.kernel);
	var elf = loadElf(new Uint8Array(buf));
	for (i = 0; i < elf.segments.length; i++) {
		seg = elf.segments[i];
		mem.loadBytes(seg.vaddr, seg.data);
	}

	var cpu = new Cpu(mem);
	cpu.pc = Number(elf.entry);
	/* Real hardware/QEMU leaves sp garbage at reset -- boot/riscv64_boot.S
	 * is responsible for setting it itself (see kernel/arch/riscv64_memmap.h).
	 * We deliberately do NOT set x2 here, so a bug in that assumption
	 * would surface the same way it would on real hardware. */

	var batchSize = 10000;
	var startedAt = Date.now();
	for (; n < args.maxInstructions;) {
		var batch = Math.min(batchSize, args.maxInstructions - n);
		cpu.run(batch, args.timeAdvance);
		n += batch;
		/* Required strings can only become true when the UART emits output.
		 * BusyBox executes millions of instructions between characters, so
		 * rescanning the whole transcript after every instruction was pure
		 * harness overhead. */
		if (output.length !== checkedOutputLength) {
			checkedOutputLength = output.length;
			var complete = true;
			for (i = 0; i < args.mustContain.length; i++)
				if (output.indexOf(args.mustContain[i]) < 0) complete = false;
			if (complete)
				break; /* early exit once everything we're waiting for has shown up */
		}
	}
	var elapsedSeconds = (Date.now() - startedAt) / 1000;

	return { output: output, instructionsRun: n, elapsedSeconds: elapsedSeconds, cpu: cpu };
}

function main() {
	var args = parseArgs(process.argv.slice(2));
	console.error('--- booting ' + args.kernel + ' under the JS emulator (max ' + args.maxInstructions + ' instructions) ---');
	var result = boot(args);
	var output = result.output, instructionsRun = result.instructionsRun;
	var elapsedSeconds = result.elapsedSeconds, failures = [], i, needle;

	console.error('--- captured UART output ---');
	console.error(output || '(empty)');
	console.error('--- end UART output ---');
	console.error('(' + instructionsRun + ' instructions executed in ' + elapsedSeconds.toFixed(1) + 's)');

	if (!output.trim())
		failures.push('no UART output at all');
	for (i = 0; i < args.mustContain.length; i++) {
		needle = args.mustContain[i];
		if (output.indexOf(needle) < 0) failures.push('expected string not found: ' + JSON.stringify(needle));
	}
	for (i = 0; i < args.mustNotContain.length; i++) {
		needle = args.mustNotContain[i];
		if (output.indexOf(needle) >= 0) failures.push('forbidden string found: ' + JSON.stringify(needle));
	}
	if (args.maxSeconds !== null && elapsedSeconds > args.maxSeconds)
		failures.push('speed regression: took ' + elapsedSeconds.toFixed(1) + 's, budget was ' + args.maxSeconds + 's');

	if (failures.length) {
		console.error('\nFAIL (' + failures.length + ' assertion(s) failed):');
		for (i = 0; i < failures.length; i++) console.error('  - ' + failures[i]);
		process.exit(1);
	}
	console.error('\nPASS (' + args.mustContain.length + ' required string(s) found, ' + args.mustNotContain.length + ' forbidden string(s) absent' + (args.maxSeconds !== null ? ', ' + elapsedSeconds.toFixed(1) + 's <= ' + args.maxSeconds + 's budget' : '') + ')');
}

if (require.main === module)
	main();

module.exports = { boot: boot, parseArgs: parseArgs };
