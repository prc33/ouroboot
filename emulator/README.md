# emulator/ — plan set, implementation starting

See `../docs/emulator-plan.md` for the actual plan (locked decisions,
phased exit criteria, the ISA subset derived from this repo's own
build output rather than assumed). Short version: a clean-room RV64
core in plain JavaScript, jor1k
(https://github.com/s-macke/jor1k/wiki/Technical-details) as reference
for technique rather than a fork base, first milestone is booting
`kernel/kernel.elf` (riscv64) headlessly under Node and matching
`kernel/test/boot_test.py`'s own assertions, browser (`xterm.js`) and
real Linux support come later and are explicitly out of scope for the
first milestone.

`docs/emulator-ecosystem-blueprint.md` is the raw external input that
prompted this plan, kept for reference.

`../docs/self-hosting-system-plan.md`'s original emulator design
(P1/P2 phases: CPU core, then differential testing against QEMU
instruction-by-instruction) predates both the RISC-V pivot and this
plan -- the phased *approach* (core first, verify against a real
reference before trusting it, add devices incrementally) still holds
and shaped `docs/emulator-plan.md`; the specific instruction-set
target and implementation language do not.
