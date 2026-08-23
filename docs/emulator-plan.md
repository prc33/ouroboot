# RISC-V browser emulator: plan

Prompted by the user's own summary of this project put to another
agent, which produced a blueprint (kept for reference:
`docs/emulator-ecosystem-blueprint.md`) proposing jor1k
(https://github.com/s-macke/jor1k/wiki/Technical-details) as a
starting point for the browser-side CPU core, plus a stretch goal of
running real Linux (built with our own TCC) alongside the custom
kernel. This doc adapts that blueprint into this project's own
conventions (locked decisions, phases with a single exit criterion
each, derive-the-spec-before-building) and reconciles it with the
existing plan.

## Relationship to the existing plan and locked decisions

`docs/self-hosting-system-plan.md`'s locked decisions still hold,
riscv64-adjusted (that doc predates the riscv64 pivot; see
`docs/riscv-port-findings.md` for the reasoning already recorded
elsewhere). One addition, not a reversal:

| # | Decision | Why |
|---|---|---|
| E1 | **jor1k is prior art / reference, not a dependency or a fork base.** Build a clean-room core in this repo, sized to exactly what our own toolchain and kernel produce (see "Derive the ISA subset" below) rather than jor1k's full RV32/RV64 + Linux-capable surface. | jor1k solves a harder problem than we have yet (full Linux, multiple ISA widths, virtio). Copying its scope would mean building and debugging device models (virtio-block, PLIC, CLINT-as-MMIO) our own kernel doesn't use yet. Its wiki/source stay a reference for technique (trap dispatch shape, MMU walk, device interfacing), not a codebase we inherit bugs from. |
| E2 | **JavaScript, not WebAssembly, for the core.** Typed arrays (`Uint32Array`/`DataView`) for memory and registers, no build step. | Matches jor1k's own choice and this project's "minimal toolchain dependency" bias elsewhere (e.g. TCC itself has no external assembler). A WASM core (via Emscripten or similar) is a plausible later optimization once the JS core's instruction-histogram profile says it's worth it -- not a starting assumption. |
| E3 | **Real Linux is a stretch goal, after the custom kernel runs in-browser -- not a parallel track.** | Matches the blueprint's own stated pitfall list almost verbatim: Linux's riscv arch code leans on GCC-specific extensions and macro-heavy headers our stripped TCC doesn't support, and auditing/patching that is a substantial, separable effort. `kernel/` already runs in-browser first is the higher-leverage, lower-risk order -- it's the same code already verified end-to-end under QEMU (`docs/riscv-port-findings.md`), so the emulator work is isolated to "does the CPU/device model behave like real hardware," not "does the kernel also need new features." |
| E4 | **No M-mode/OpenSBI emulation for the custom-kernel target.** Start the hart directly in S-mode, PC at the kernel's entry point. | `kernel/`'s riscv64 port makes zero SBI ecalls (confirmed: it uses the Sstc extension's `time`/`stimecmp` CSRs directly for timing, not `sbi_set_timer`) and never touches PMP or M-mode CSRs -- QEMU's own OpenSBI firmware is a pass-through for this kernel, not a real dependency. Skipping it entirely removes an entire privilege level's worth of CSRs and traps from the *first* milestone. Required again once Linux (E3) is in scope -- Linux's boot protocol does expect SBI. |

## Derive the ISA subset -- before writing the decoder

Same principle as the original plan's P0 ("derive the specs
empirically... freeze all three as checked-in files"), applied here.
Checked directly against this repo's own build output rather than
assumed:

```
$ riscv64-linux-gnu-objdump -d kernel/kernel.elf | <mnemonic histogram>
add addi addiw addw and andi auipc beq beqz blt bltu bne bnez
csrr csrrw csrw divu ebreak j jalr jr lbu ld lhu li lui lw mul
mulw mv nop not or ori rdtime remu remuw remw ret sb sd seqz
sext.w sfence.vma slli slliw sllw slti sltiu sret srl srli
srliw srlw sub subw sw unimp wfi xor
```

No `f*` (float), no `c.*` (compressed), no `amo*`/`lr.*`/`sc.*`
(atomics) -- confirmed by the same histogram technique against the
*kernel* binary. The self-hosted compiler binary (`compiler/stage1/tcc`,
riscv64-gen.c's own codegen for `double`) does use real hardware `F`/`D`
instructions (`fadd.d`, `fld`, `fsw`, etc.) -- so real userspace
binaries (busybox, TCC itself) need F/D support, just not the kernel.
This cleanly splits the ISA surface into two milestones instead of one
guess:

- **Milestone A (kernel-only): RV64IM + Zicsr + the privileged subset
  above (`sret`/`ecall`/`wfi`/`sfence.vma`) + Sstc (`rdtime`,
  `stimecmp`).** No F/D, no compressed, no atomics, no M-mode.
- **Milestone B (real userspace binaries): adds F/D** (double only has
  been sufficient so far -- single-precision appears in the histogram
  too, `fmv.s`/`fcvt.s.*`, so both). Atomics (`A`) and compressed (`C`)
  stay deferred until something actually needs them (no threads/futex
  yet, and neither our compiler nor musl's build here emit compressed
  instructions -- reconfirm before ever adding either, don't assume).

## Phases

### P0 -- ISA histogram (done, see above)
**Exit:** the mnemonic list above, derived not assumed. Re-run and
diff if the kernel or userspace binaries change meaningfully.

### P1 -- Headless core, Milestone A ISA, boots `kernel.elf`
Decode/execute for the histogram above, a flat `ArrayBuffer`-backed
physical memory (sized to cover `RV64_RAM_BASE`..`RV64_MEM_TOP` from
`kernel/arch/riscv64_memmap.h`), an ELF64 loader (parse `kernel.elf`'s
`PT_LOAD` segments -- same format `mm/elf.c` already parses on the
kernel side, independent implementation here since this runs in JS,
not under the kernel), CSR file, Sv39 MMU (needed as soon as the
kernel's own `paging_init` runs), and the UART device at `0x10000000`
(same MMIO layout `drivers/riscv64_serial.c` expects). Start the hart
in S-mode per E4, sp garbage (matching real hardware/QEMU -- the
kernel's own `boot/riscv64_boot.S` is responsible for setting it, same
as under QEMU).

Node.js first (fast iteration, no browser needed), the same JS module
usable from a browser `<script>` later -- no browser-specific API
(DOM, `fetch`) in the core itself.

**Exit:** the emulator's captured UART output contains the same
strings `kernel/test/boot_test.py` already asserts on for
`ARCH=riscv64`, run against the *same* `kernel.elf` QEMU boots --
reusing that checklist directly is the differential-testing principle
from the original plan's P2, applied at the assertion-string level
since a true register/memory lockstep against QEMU (as the original
i386 plan intended) is a larger undertaking than this milestone needs
yet.

### P2 -- Full kernel test parity
Every checkpoint `make ARCH=riscv64 test` checks (trap/`ebreak`, Sv39
+ COW, Sstc timer driving the scheduler, `ecall` syscalls +
S-mode/U-mode transitions, the real ELF loader running a real musl+TCC
binary) passes under the JS emulator too. This is where MMU
correctness and CSR trap-and-return semantics actually get exercised
hard -- the scheduler and syscall checkpoints are the ones most likely
to surface a subtly wrong trap/CSR implementation, per
`docs/riscv-port-findings.md`'s own experience with exactly that class
of bug on the kernel side.

**Exit:** `kernel/test/boot_test.py`-equivalent assertions all pass
under the emulator, for every checkpoint, not just boot.

### P3 -- Browser I/O
`xterm.js` over the same UART byte-in/byte-out interface (matches the
original plan's D8 and `emulator/README.md`'s existing framing --
nothing new here, just finally implementing it), a Web Worker running
the CPU loop so terminal input doesn't block on it.

**Exit:** `kernel.elf`'s P5-checkpoint-2 output ("hello from musl+tcc,
sum=285") visible in an actual browser tab, not just Node's stdout.

### P4 -- Milestone B ISA (F/D), real busybox in-browser
Add F/D decode/execute (double first -- confirmed as what the
self-hosted compiler needs; single-precision alongside it since both
appear in the same histogram). Build busybox with the riscv64 TCC+musl
pipeline (`demo/build-busybox-riscv64.sh`, already proven), get it
running as a process under the kernel -- which needs `fork`/`exec`
and a real filesystem/ramfs the kernel doesn't have yet (a kernel-side
gap, not an emulator one; tracked here because it's the next thing
this phase's exit criterion depends on, but the actual work is a
`kernel/` checkpoint in its own right, sized similarly to the
i386 plan's P6).

**Exit:** `busybox ash` running interactively in the browser terminal,
piping between real coreutils applets.

### P5 -- Self-hosting in-browser (the actual "holy grail" from the
attached blueprint)
`tcc` (built by our host toolchain) placed in the boot image, run
*inside* the browser-emulated kernel, recompiling its own source.
This is this project's actual P9 closure milestone
(`docs/self-hosting-system-plan.md` section 4), reached inside the
browser rather than just on bare QEMU.

**Exit:** `tcc -o tcc_native tcc.c` succeeds inside the in-browser
shell, and `tcc_native` correctly recompiles itself again
(byte-identical second-generation output, same bar the original plan
sets).

### P6 (stretch, per E3) -- Real Linux
Only after P5. Audit Linux's riscv64 arch code against what our TCC
actually supports (same technique as the compiler port: try building,
catalog every real failure, fix or stub each one, document in a
findings doc same as everything else in `docs/`) rather than assuming
scope up front. Needs M-mode/SBI emulation reinstated (E4 was
kernel-specific), a real block/virtio device instead of an initramfs-
only ramfs, and almost certainly a much larger patch surface against
upstream Linux than musl or busybox needed. No line-count or time
budget assumed yet -- this is exactly the kind of estimate the
project's own methodology says to derive from a real attempt, not
guess in advance.

## Verified so far

Nothing yet -- this doc is the plan; `emulator/README.md` points here.
Implementation work (P1) starts immediately after this plan is
committed; findings go in a new `docs/emulator-p1-findings.md` etc.,
same convention as every other phase in this repo.
