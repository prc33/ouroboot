# cpu/ — not started

Planned: an FPGA implementation of the same RISC-V core the emulator
and kernel target, on a small, cheap, open-toolchain board (Lattice
ECP5 was the leading candidate discussed -- see the project history
for the reasoning: open yosys/nextpnr/prjtrellis toolchain, no vendor
lock-in, and real precedent of MMU-capable RISC-V soft cores fitting
on this class of hardware).

Deliberately scoped to RISC-V rather than i386 or ARM for this
specific goal: RISC-V's base ISA is designed to be minimal and is
royalty-free/open by design, unlike ARM (real licensing questions
around synthesizing ARM-compatible cores) or x86 (no comparable
precedent of hobbyist from-scratch implementation at this tier).

Nothing here yet. The realistic starting point once this begins:
`docs/riscv-port-findings.md`'s instruction-usage findings (which
operations TCC's own codegen and the kernel's inline-asm-turned-
intrinsics actually need) directly scope what the core has to
implement, the same way the i386 instruction-count measurement scoped
the original emulator discussion.
