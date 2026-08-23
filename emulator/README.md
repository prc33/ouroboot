# emulator/ — P1+P2 done: boots `kernel/kernel.elf`, full test parity with QEMU

See `../docs/emulator-plan.md` for the plan (locked decisions, phased
exit criteria, the ISA subset derived from this repo's own build
output) and `../docs/emulator-p1-findings.md` for what's built and the
one real bug found bringing it up.

`js/` is a from-scratch RV64IM + Zicsr + privileged-subset interpreter
in plain JavaScript (jor1k -- https://github.com/s-macke/jor1k/wiki/Technical-details
-- as reference for technique, not a fork base or dependency). Try it:

```
cd ../kernel && make ARCH=riscv64        # build kernel.elf
make ARCH=riscv64 test-js                # boot it under this emulator
```

or directly:

```
node js/boot.js ../kernel/kernel.elf --must-contain "P5 checkpoint 2 OK"
```

`docs/emulator-ecosystem-blueprint.md` is the raw external input that
prompted `docs/emulator-plan.md`, kept for reference.

Next up: P3 (`xterm.js` in an actual browser tab instead of Node) and
P4 (F/D instruction support, needed for real userspace binaries like
the self-hosted compiler, beyond what the kernel itself uses).

`../docs/self-hosting-system-plan.md`'s original emulator design
(P1/P2 phases: CPU core, then differential testing against QEMU
instruction-by-instruction) predates both the RISC-V pivot and
`docs/emulator-plan.md` -- the phased *approach* still holds and
shaped this plan; the specific instruction-set target and
implementation language do not.
