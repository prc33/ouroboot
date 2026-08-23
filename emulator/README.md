# emulator/ — not started

Planned: a from-scratch CPU emulator (originally i386, now pivoted to
RISC-V64 alongside the rest of the project) with a browser-facing
target (xterm.js over a serial byte interface, matching the kernel's
serial-only I/O design) and a native target for fast QEMU-differential
testing during development.

See `../docs/self-hosting-system-plan.md` for the original emulator
design (P1/P2 phases: CPU core, then differential testing against
QEMU instruction-by-instruction). That plan predates the RISC-V pivot
and was written with i386 in mind -- the phased *approach* (core
first, differential-test against QEMU before trusting it, add devices
incrementally) still applies; the specific instruction-set target does
not.
