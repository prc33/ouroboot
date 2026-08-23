#!/usr/bin/env python3
"""
Boot-and-assert test harness for the kernel.

Why this exists: the kernel has no display, no debugger attached by
default, and a hung/crashed kernel otherwise just... sits there. This
script is the load-bearing piece of "how do we test the kernel as we
go" from the project plan. Every phase (P3 onward) should add its own
assertions here rather than eyeballing QEMU output by hand.

Usage:
    boot_test.py <kernel.elf> [--must-contain STR ...] [--must-not-contain STR ...]
                              [--timeout SECONDS] [--mem MB]

Exit code 0 = all assertions passed. Nonzero = failure, with a report
of exactly which assertion failed and the full captured serial log
(so a failure is immediately diagnosable, not a mystery hang).
"""
import argparse
import subprocess
import sys
import shlex

DEFAULT_TIMEOUT = 10
DEFAULT_MEM_MB = 32


def boot_and_capture(kernel_path, timeout, mem_mb, extra_qemu_args=None, qemu_binary="qemu-system-i386"):
	cmd = [
		"timeout", str(timeout),
		qemu_binary,
		"-kernel", kernel_path,
		"-serial", "stdio",
		"-display", "none",
		"-no-reboot",
		"-m", str(mem_mb),
	]
	if extra_qemu_args:
		cmd += extra_qemu_args
	proc = subprocess.run(cmd, capture_output=True, text=True)
	# exit code 124 from `timeout` means we hit the wall clock, which is
	# EXPECTED for any kernel that halts forever after printing (no
	# graceful shutdown mechanism exists yet) -- so we treat "output
	# captured, then timeout" as a normal, successful run, not a failure
	# in itself. Only the assertions below decide pass/fail.
	return proc.stdout, proc.returncode


def run(kernel_path, must_contain, must_not_contain, timeout, mem_mb, extra_qemu_args=None, qemu_binary="qemu-system-i386"):
	print(f"--- booting {kernel_path} with {qemu_binary} (timeout={timeout}s, mem={mem_mb}MB) ---")
	output, qemu_exit = boot_and_capture(kernel_path, timeout, mem_mb, extra_qemu_args, qemu_binary)

	failures = []

	if not output.strip():
		failures.append(
			"no serial output at all -- kernel likely triple-faulted, "
			"hung before serial_init(), or QEMU failed to load the image"
		)

	for needle in must_contain:
		if needle not in output:
			failures.append(f"expected string not found: {needle!r}")

	for needle in must_not_contain:
		if needle in output:
			failures.append(f"forbidden string found: {needle!r}")

	print("--- captured serial output ---")
	print(output if output.strip() else "(empty)")
	print("--- end serial output ---")

	if failures:
		print(f"\nFAIL ({len(failures)} assertion(s) failed):")
		for f in failures:
			print(f"  - {f}")
		return 1

	print(f"\nPASS ({len(must_contain)} required string(s) found, "
	      f"{len(must_not_contain)} forbidden string(s) absent)")
	return 0


def main():
	ap = argparse.ArgumentParser(description=__doc__,
	                              formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("kernel", help="path to kernel ELF (multiboot)")
	ap.add_argument("--must-contain", action="append", default=[],
	                 help="substring that must appear in serial output (repeatable)")
	ap.add_argument("--must-not-contain", action="append", default=[],
	                 help="substring that must NOT appear in serial output (repeatable)")
	ap.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT)
	ap.add_argument("--mem", type=int, default=DEFAULT_MEM_MB, dest="mem_mb")
	ap.add_argument("--qemu-arg", action="append", default=[], dest="extra_qemu_args",
	                 help="extra raw arg to pass to the qemu binary (repeatable)")
	ap.add_argument("--qemu-binary", default="qemu-system-i386",
	                 help="qemu system binary to boot under (default: qemu-system-i386)")
	args = ap.parse_args()

	rc = run(args.kernel, args.must_contain, args.must_not_contain,
	         args.timeout, args.mem_mb, args.extra_qemu_args, args.qemu_binary)
	sys.exit(rc)


if __name__ == "__main__":
	main()
