# emulator/ — P1+P2+P3 done: boots kernel/kernel.elf, in a real browser tab, full test parity with QEMU

See `../docs/emulator-plan.md` for the plan (locked decisions, phased
exit criteria, the ISA subset derived from this repo's own build
output) and the findings docs for what's built and the real bugs found
bringing each phase up: `../docs/emulator-p1-findings.md` (headless
Node, the core CPU/MMU/UART) and `../docs/emulator-p3-findings.md`
(the browser/`xterm.js`/Worker wrapper).

`js/` is a from-scratch RV64IM + Zicsr + privileged-subset interpreter
in plain JavaScript (jor1k -- https://github.com/s-macke/jor1k/wiki/Technical-details
-- as reference for technique, not a fork base or dependency).

**Headless (Node), matching `kernel/test/boot_test.py`'s own checkpoints:**
```
cd ../kernel && make ARCH=riscv64        # build kernel.elf
make ARCH=riscv64 test-js                # boot it under this emulator
```

**In an actual browser tab:**
```
cd ../kernel && make ARCH=riscv64        # build kernel.elf (must be riscv64 --
                                          # kernel.elf is a shared build-artifact
                                          # filename across ARCH= targets, see
                                          # kernel/Makefile's own top comment)
cd ../emulator/js && python3 -m http.server 8000
# open http://localhost:8000/index.html
```
(Any static HTTP server works -- `fetch()` and Worker script loading
both need a real origin, not `file://`.)

`docs/emulator-ecosystem-blueprint.md` is the raw external input that
prompted `docs/emulator-plan.md`, kept for reference.

Next up: P4 (F/D instruction support, needed for real userspace
binaries like the self-hosted compiler, beyond what the kernel itself
uses).

`../docs/self-hosting-system-plan.md`'s original emulator design
(P1/P2 phases: CPU core, then differential testing against QEMU
instruction-by-instruction) predates both the RISC-V pivot and
`docs/emulator-plan.md` -- the phased *approach* still holds and
shaped this plan; the specific instruction-set target and
implementation language do not.
