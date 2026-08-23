# A Self-Hosting System in ~25k Lines

**Goal.** Three pieces of code we own — an i386 emulator, a C compiler, a kernel — such that the compiler can rebuild itself and the kernel while running inside the emulator, and the whole thing runs in a browser tab with a working shell, network, and container support.

**Closure condition.** TCC compiles TCC, inside our kernel, inside our emulator. Everything else is packaging.

---

## 0. Locked decisions

These are settled. Reopening any of them changes the budget materially, so each is recorded with the reason.

| # | Decision | Why |
|---|---|---|
| D1 | **i386 only.** No x86-64, no long mode. | No REX, 2-level paging not 4, simpler ABI. Roughly thirds the emulator. TCC's oldest, most solid backend. |
| D2 | **No JavaScript backend for TCC.** Browser is reached by compiling the *emulator* with Emscripten. | A JS target can't host a kernel — no page tables, no privileged instructions, no hand-written asm. |
| D3 | **Our own kernel, not Linux.** | Real Linux needs `asm goto` + a large `__builtin_*` surface + alternatives patching + objtool. tccboot did it against 2.4.26 in 2004 and nobody has repeated it. It also inflates the emulator: Linux probes CPUID leaves, MSRs, TSC, FPU, legacy devices we'd otherwise never implement. |
| D4 | **Syscall ABI is binary-compatible with Linux i386.** Same numbers, same `int 0x80`, same struct layouts. | The same static musl binary runs on Linux and on us. `strace` on Linux becomes our spec and our differential oracle for userspace. |
| D5 | **TCP lives in userspace.** Kernel exposes a raw Ethernet frame device; lwIP in `NO_SYS=1` raw-callback mode links into the app. | Kernel gets *smaller* (~100 lines vs ~300 for a socket shim). Real congestion control, ping, DNS for free. Avoids promoting `clone(CLONE_VM)` to a hard requirement, which lwIP's BSD socket layer would force. |
| D6 | **Processes, not threads.** `clone(CLONE_VM)` is a stretch goal. | bash, busybox, curl, TCC are all single-threaded. But musl still needs `set_thread_area`, `set_tid_address`, `futex` — those are in scope. |
| D7 | **ramfs only.** No block device, no on-disk format. | Boot from a tar image; `tar` state out over the network for persistence. |
| D8 | **Serial console only.** 16550 UART. | ~150 lines vs ~1500 for VGA + 8042. The terminal is HTML anyway. |
| D9 | **x87 backed by host `double`.** No 80-bit extended precision. | TCC's i386 backend emits x87 for all FP and busybox `awk` is double-based. Known leak: musl's `printf` uses `long double` on i386, so `%.17g` output can differ in the last digit. Accepted. |
| D10 | **Differential testing against QEMU is the primary debugging method.** | Converts "hang at instruction 40M" into "instruction 3,921,004: `shrd` sets AF wrong." Highest-leverage tooling in the project. |
| D11 | **Containers are `chroot` + optional namespaces, not a security boundary.** | No user ns, no seccomp, no capabilities. Framing is "chroot with better ergonomics." |

---

## 1. Scope boundary

### In scope — we write and maintain this

```
emulator/   ~7,000 lines    C89-ish, builds native + Emscripten
tcc/        ~11,000 lines   fork of mainline TCC, stripped to i386
kernel/     ~7,000 lines    C89-ish, TCC-compilable, self-hostable
                            ─────────
                            ~25,000 lines
```

Hard constraint on `kernel/` and `emulator/`: **must compile with our own TCC.** C89-ish, no `asm goto`, no statement expressions in load-bearing places, inline asm confined to a dozen accessor macros in one header. If it doesn't self-host, the closure doesn't close.

### Out of scope — external, unmodified where possible

| Project | Role | Notes |
|---|---|---|
| **musl** | libc | Riskiest external. `weak_alias` needs `__attribute__((weak,alias))`; i386 syscall stubs use asm constraint forms TCC handles unevenly. |
| **busybox** | coreutils, `ash`, `awk`, `sed`, `tar`, `gunzip` | Strip `-ffunction-sections -Wl,--gc-sections` and section-sorting from Kbuild — TCC's linker has no section GC. Accept a fatter binary. |
| **bash** | interactive shell, job control | Portable. Exercises the tty layer harder than anything else. |
| **lwIP** | TCP/IP, `NO_SYS=1` | Plus a ~300-line single-threaded socket shim (also external) mapping `connect`/`send`/`recv` onto the raw API, pumping `sys_check_timeouts()` and the frame device. |
| **mbedTLS** | TLS for https | Builds under TCC. Terminating TLS at the relay instead would be cheating. |
| **curl** | http client | Hand-rolled Makefile. Do not run autoconf in the box. |
| **PDPmake** | `make` for in-box builds | busybox has no `make`. |
| **libslirp** *(native)* / **WebSocket relay** *(browser)* | host-side Ethernet | D5 moves TCP into our userspace but the *host* now owes us framing. |
| **OCI pull script** | container images | ~100 lines of shell over busybox + curl. No daemon, no Go. |

### Explicitly not doing

SMP · block devices/disk filesystems · VGA/8042 · BIOS/real mode/bootloader · dynamic linking (static only) · x86-64 · user namespaces, seccomp, capabilities · vDSO/`AT_SYSINFO` · glibc · autoconf inside the emulator · 80-bit long double · raw sockets from a container.

---

## 2. Line-of-code targets

Targets are for design pressure, not accounting. If a component exceeds its target by >50%, that's a signal to re-scope, not to pad the budget.

### 2.1 Emulator — 6,550 target / 7,000 ceiling

| Component | Target | Notes |
|---|---:|---|
| Instruction decode + execute | 3,000 | The real spec is the mnemonic histogram from §3, not "what TCC emits". |
| Flags (incl. PF, AF, `shrd`/`shld`, `bt*`) | *within above* | Single largest source of subtle divergence. Table-driven PF. |
| MMU: 2-level paging, TLB, page faults | 400 | |
| Exceptions, IDT dispatch, `int`/`iret` | 250 | |
| PIC (8259) + PIT (8254) | 150 | |
| x87 FPU (host `double` backed) | 1,000 | D9. |
| UART 16550 | 150 | Byte-in/byte-out interface — the seam the browser plugs into. |
| NIC (two-ring MMIO, raw frames) | 150 | |
| RTC / CMOS | 50 | |
| ELF + boot image loader | 200 | |
| QEMU differential harness | 500 | Native-only, excluded from Emscripten build. |
| Tracer / gdb stub | 400 | |
| Emscripten + xterm.js glue | 300 | Web Worker + `SharedArrayBuffer` so input doesn't block the CPU loop. |

### 2.2 TCC — 11,000 target

This is a **fork-and-strip**, not a rewrite. Mainline is ~20k across all targets.

Delete: x86-64, ARM, ARM64, RISC-V backends · PE/COFF + Windows · bounds checking · DWARF and stabs emitters · unused `-m` variants.

Keep, non-negotiably: **the integrated assembler.** Our kernel's accessor macros and musl's syscall stubs are inline asm; without it we shell out to binutils and lose self-containment. This is why chibicc-class simplicity (~9k) is a mirage — it can't build musl or our kernel.

| Component | Target |
|---|---:|
| Preprocessor | 2,500 |
| Parser + type system | 3,500 |
| i386 codegen | 2,000 |
| Integrated assembler + inline asm | 1,800 |
| ELF output + linker | 1,200 |

### 2.3 Kernel — 6,560 target / 7,000 ceiling

| Component | Target | Notes |
|---|---:|---|
| Boot, GDT, IDT, TSS | 400 | |
| Physical page allocator | 200 | |
| Paging + copy-on-write | 700 | COW is not optional: bash forks per subshell and needs real copy semantics. |
| Scheduler, context switch, timer preemption | 400 | Round-robin. No priorities, no load balancing. |
| Process table, `fork`/`exec`/`wait` | 800 | |
| ELF loader | 250 | |
| Signals | 350 | Incl. `SIGINT`, `SIGTSTP`, `SIGCHLD`, sigreturn trampoline. |
| ramfs | 500 | |
| VFS, fd table, path resolution | 600 | Per-process root+cwd from day one (see containers). |
| Pipes | 150 | |
| **tty: line discipline, canonical mode, job control** | 600 | Process groups, `tcsetpgrp`, controlling terminal. **Most underestimated piece in the project.** |
| `/dev/net` frame chardev | 100 | `read`/`write` a frame, `poll` for readable. |
| Syscall dispatch table | 250 | ~60–70 syscalls, derived empirically in §3. |
| futex (wait/wake) | 150 | musl needs it even single-threaded. |
| `set_thread_area` / TLS GDT entry | 80 | Per-process `%gs` for musl TLS. |
| `brk` / `mmap` / `munmap` | 300 | Anonymous only. |
| **Containers** | **730** | |
| — `chroot` + per-process root | 50 | ~80% of the practical value. Path resolution refuses to walk above root. |
| — UTS namespace | 30 | |
| — PID namespace, **one level only** | 250 | ns-local pid alongside global; translate at `getpid`/`wait`/`kill`; ns-init reaps orphans. One level ⇒ a pointer and an int, not a tree walk. |
| — mount namespace | 250 | Private `/proc`, `/tmp`. |
| — memory cgroup | 150 | Page cap at the allocator. |
| — network namespace | **0** | Payoff from D5: isolation = which fd you hold. Don't hand the container `/dev/net`, or hand it a second frame queue. A userspace switch bridges them if needed. |

---

## 3. Derive the specs empirically, before writing code

Three histograms replace three guesses. Do these first; they're cheap and they scope everything downstream.

1. **Instruction set.** Build musl, busybox, bash, curl, TCC for i386. `objdump -d` everything. Histogram mnemonics + operand forms. Expect a few hundred forms, not thousands. That list is the emulator's decode spec.
2. **Syscall surface.** `strace` the same binaries doing real work (interactive bash session with job control, `curl https://...`, `tcc -c` on itself). Histogram syscall numbers. Expect 60–70.
3. **x87 subset.** Grep the disassembly for `f*` mnemonics. Likely a small fraction of the full x87 set.

Freeze all three as checked-in files. Anything outside them traps to a loud "unimplemented" with a mnemonic/number in the message.

---

## 4. Phases

Each phase has a single exit criterion. Do not proceed on a partial pass.

### P0 — Specs
Run §3. **Exit:** three frozen histogram files in the repo.

### P1 — Emulator core, headless, native
Decode/execute, MMU, exceptions. No devices. **Exit:** runs a static i386 `hello` from the host under a stub syscall layer.

### P2 — Differential testing
Lockstep against QEMU, comparing registers + touched memory per instruction. **Exit:** 10⁹ instructions of mixed workload with zero divergence. *This is the highest-value phase in the project; do not shortcut it.*

### P3 — Kernel boots, prints
UART, GDT/IDT, `printf` from `main`. **Exit:** banner on serial.

### P4 — Memory + scheduling
Physical allocator, paging, COW, timer preemption between two hardcoded kernel tasks. **Exit:** two tasks alternating on timer IRQ; COW unit test passes.

### P5 — First userspace
ramfs, VFS, ELF loader, syscall dispatch. **Exit:** static musl `hello` runs from ramfs. **Same binary must run unmodified on Linux** (D4).

### P6 — Multiprocess
`fork`/`exec`/`wait`, pipes, signals. **Exit:** busybox `ash` runs `ls | wc -l` correctly.

### P7 — tty and job control
Line discipline, process groups, controlling terminal. **Exit:** bash with working `^C`, `^Z`, `fg`, `bg`, and correct `SIGTTOU` behaviour on background writes.

### P8 — Network
Emulator NIC, `/dev/net`, lwIP `NO_SYS=1` + socket shim, host-side slirp. **Exit:** `curl http://...` succeeds. Then mbedTLS → `curl https://...`.

### P9 — **CLOSURE**
Get TCC + PDPmake into the ramfs. **Exit:** `tcc` rebuilds `tcc` inside the kernel inside the emulator, and the resulting binary rebuilds itself again — byte-identical to the first output.

### P10 — Browser
Emscripten, xterm.js, Web Worker + `SharedArrayBuffer`, WebSocket relay doing slirp's job. **Exit:** P9 reproduced in a browser tab.

### P11 — Containers
`chroot` → UTS → PID ns → mount ns → memcg, in that order. **Exit:** `i386/alpine` pulled from a registry by the shell script, chrooted, running its own busybox with its own PID 1.

---

## 5. Build order for externals

Strictly sequential; each unblocks the next.

```
musl → busybox → tcc-builds-tcc → bash → PDPmake → lwIP+shim → mbedTLS → curl
```

**Cross-build everything from the host using the identical TCC binary.** Self-host only projects with hand-written Makefiles. Do not run autoconf inside the emulator — it spawns thousands of processes before producing a single object file, and proves nothing except that the tty layer is slow.

---

## 6. Risk register

| Risk | Severity | Mitigation |
|---|---|---|
| **musl won't build under TCC** (`weak_alias`, asm constraints) | **Blocks everything** | Attempt in week 1, before any kernel work. Fallback: patch musl's `weak_alias` to plain aliases; hand-write the i386 syscall stubs. |
| Flag semantics divergence (PF, AF, `shrd`/`shld`, `bt*`) | High | P2 differential testing is the entire answer. |
| tty/job control underestimated | High | Dedicated phase (P7). Test against a real bash session transcript captured on Linux. |
| TCC inline asm vs musl expectations | High | Same week-1 spike as row 1. |
| x87 `long double` in musl `printf` | Low | Accepted (D9). Document the `%.17g` last-digit difference; don't debug it twice. |
| Container images unavailable for i386 | Medium | Target `i386/alpine` — musl-based, same libc we already validate. glibc images would need vDSO + a much wider syscall surface; out of scope. |
| Only one process gets the network (D5) | Low | Fine for a single-user box. Escape hatch: a `netd` owning `/dev/net`, others talk to it over a pipe. |
| Emscripten build diverges from native | Medium | Keep the CPU core free of host-specific code; CI both targets from P1. |

---

## 7. Immediate next actions

1. Spike musl-under-TCC on the host. Timebox: one week. **This gates the project.**
2. Run the three histograms (§3), freeze them.
3. Fork TCC, strip to i386, confirm it still self-hosts on the host. Measure actual line count against the 11k target.
4. Stand up the QEMU differential harness skeleton *before* writing the second instruction.
