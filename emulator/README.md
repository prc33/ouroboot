# emulator/ — P1+P2+P3 done: boots kernel/kernel.elf, in a real browser tab, full test parity with QEMU; checkpoint 10's real interactive busybox ash accepts real typed input, in the browser too

See `../docs/emulator-plan.md` for the plan (locked decisions, phased
exit criteria, the ISA subset derived from this repo's own build
output) and the findings docs for what's built and the real bugs found
bringing each phase up: `../docs/emulator-p1-findings.md` (headless
Node, the core CPU/MMU/UART) and `../docs/emulator-p3-findings.md`
(the browser/`xterm.js`/Worker wrapper).

`js/` is a from-scratch RV64IM + the required F/D, Zicsr, and
privileged-subset interpreter in plain JavaScript (jor1k --
https://github.com/s-macke/jor1k/wiki/Technical-details -- as reference
for technique, not a fork base or dependency). RV64 registers, CSRs,
floating-point bits, and PTEs use paired 32-bit typed arrays; the hot
path contains no `BigInt`, `DataView`, classes, arrow functions, or
other syntax outside MQuickJS's strict subset. Every `js/*.js` file is
accepted by the official MQuickJS compiler.

The performance regression target is the real scripted BusyBox shell,
not a synthetic instruction loop:
```
cd ../kernel && make ARCH=riscv64 shell-js
```
On the development Node/V8 host, the paired-word core executes the
100.78-million-instruction shell scenario in about 6.4 seconds and the
225.11-million-instruction full checkpoint suite in about 15 seconds.
The exact rate is host-dependent; `shell-js` enforces a conservative
30-second ceiling.

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
cd ..    # repo root -- index.html fetches ../../kernel/kernel.elf,
         # so the server's root has to be the repo root, not
         # emulator/js/ itself (a server rooted at emulator/js/ 404s
         # on that fetch -- python3 -m http.server won't serve a path
         # that escapes its own root)
python3 -m http.server 8000
# open http://localhost:8000/emulator/js/index.html
```
(Any static HTTP server works -- `fetch()` and Worker script loading
both need a real origin, not `file://`.)

Once the kernel finishes booting (P4 through P10's checkpoints, then a
real busybox ash prompt), the terminal is live: click into it and type
-- real keystrokes go through `app.js`'s `term.onData` handler, into
the Worker via `postMessage`, into the kernel's own UART RX exactly as
a real serial keyboard would, and back out as real shell output.
Verified end-to-end with Puppeteer (real headless Chromium, real
simulated per-character keystrokes, not just simulated in Node) -- see
this repo's own git history (commits `6932c25` and `4e21ca4`) for how
that was confirmed and the two real bugs it found along the way: a
nested-trap COW bug (a page fault mid-syscall corrupting this kernel's
single shared trapframe), and `sys_read` needing its own ICRNL
translation by hand, since there's no tty layer to do it the usual
way.

`docs/emulator-ecosystem-blueprint.md` is the raw external input that
prompted `docs/emulator-plan.md`, kept for reference.

`../docs/self-hosting-system-plan.md`'s original emulator design
(P1/P2 phases: CPU core, then differential testing against QEMU
instruction-by-instruction) predates both the RISC-V pivot and
`docs/emulator-plan.md` -- the phased *approach* still holds and
shaped this plan; the specific instruction-set target and
implementation language do not.
