# Backend review — where the compiler's remaining lines are not

Scope: `compiler/i386/`, `compiler/risc/`, `compiler/wasm/` — 8,709 lines, 28%
of the compiler — reviewed against two questions the 25,000-line budget raises:

1. Can the three backends share code?
2. The wasm backend is much bigger than the other two. Why, and can it shrink?

Both answers are measured, not argued. The short version is that **the backends
are not where the remaining lines are**, and the single biggest candidate for
deletion turns out to pay for itself several times over.

---

## Sizes

| Backend | Files | Lines |
|---|---|---:|
| wasm32 | `tccwasm.c` 1,900 · `wasm-gen.c` 1,081 · `wasm-backend.h` 127 · `wasm-link.c` 66 | **3,174** |
| riscv64 | `riscv64-gen.c` 1,439 · `riscv64-link.c` 320 · `riscv64-asm.c` 107 · `riscv64-softquad.c` 105 | **1,971** |
| i386 (codegen only) | `i386-gen.c` 966 · `i386-pair.c` 295 · `i386-link.c` 193 | **1,454** |
| i386 (assembler) | `i386-asm.c` 1,415 · `i386-asm.h` 480 · `i386-tok.h` 215 | 2,110 |

The assembler is out of scope here — it is being handled separately.

---

## 1. Can the backends share code?

**Essentially no, and the reason is structural rather than accidental.**

Measured two ways.

**Function-level comparison.** Of the 24 functions implemented by all three
backends, the ones that look shareable by name are not shareable by body:

| Function | i386 | rv64 | wasm | Why it differs |
|---|---:|---:|---:|---|
| `gen_vla_sp_save` | 7 | 5 | 6 | `mov %esp,off(%ebp)` vs `sd sp,off(s0)` vs unsupported |
| `gen_fill_nops` | 10 | 11 | 6 | `0x90` vs 4-byte `addi x0,x0,0` vs a zero byte |
| `ggoto` | 7 | 6 | 5 | `gcall_or_jmp(1)` vs `(0)` vs unsupported |
| `load` | 76 | 174 | 150 | entirely different addressing and register files |
| `gfunc_call` | 89 | 201 | 87 | entirely different ABIs |

These are small because the job is small, not because they are boilerplate.
The one genuine duplicate is `gjmp_append` (~14 lines, identical in i386 and
riscv64 apart from a comment); it walks a jump list threaded through
`cur_text_section->data`, which the wasm backend cannot share because it has no
text section to thread through. Hoisting it would save 14 lines and add a
declaration — not worth the indirection.

**Clone detection.** A sliding 6-line normalised-token scan across all eight
backend files finds **10 cross-file clones, all of them comment blocks or
`switch`/`case` scaffolding** — no shared logic. The relocation tables in
`i386-link.c` and `riscv64-link.c` look similar in shape but contain different
relocation numbers, different PLT encodings, and different GOT rules; they are
the definition of architecture-specific.

**Conclusion:** the arch seam is in the right place. There is no meaningful
common-utility extraction available here, and forcing one would add a layer of
indirection over three genuinely different instruction encoders.

---

## 2. Why is the wasm backend 1.6× riscv64?

Because it is doing strictly more work. The i386 and riscv64 backends translate
TCC's value stack straight into machine code as they go. wasm cannot: it is a
**structured** stack machine with no arbitrary branches, so a function's
control flow cannot be emitted until the whole function is known.

So the wasm backend is two passes:

- `wasm-gen.c` implements the ordinary TCC backend interface, but instead of
  emitting bytes it records a **register-machine IR** (`WasmOp`, 49 live opcodes
  of 50 defined — the IR carries almost no dead weight).
- `tccwasm.c` translates that IR into wasm, reconstructing structured control
  flow from arbitrary jumps, then writes the module.

That second pass is the extra ~1,200 lines, and it has no counterpart in the
other two backends. It is not redundancy.

### The stackifier: measured, and worth keeping

`wasm_emit_function_body()` contains **two complete control-flow strategies**:

- a **stackifier** (~313 lines) that reconstructs real wasm `block`/`loop`/`br`
  scopes, used when the CFG is reducible (which C almost always produces);
- a **`br_table` dispatch fallback** (~53 lines) — a `loop` around a jump table
  on a synthetic `pc` local — used when the function contains a "jump into
  loop" pattern the structured form cannot express.

Deleting the stackifier and always using the fallback is the single largest
deletion available anywhere in the backends. It was tested rather than
assumed, by forcing `use_structured = 0` and rebuilding the browser emulator:

| | Emulator boot (294.6M instructions) | `rv64.wasm` size |
|---|---:|---:|
| Stackifier (current) | **44.55 s** | **30,212 bytes** |
| Dispatch fallback only | 67.87 s | 32,427 bytes |

**52% slower, and the module gets bigger.** Those 313 lines buy a third off the
runtime of the project's headline demo, and pay for themselves in output size
too. This is not a candidate for deletion, and the measurement is recorded here
so it does not have to be re-litigated.

### What the emitter switch is not

`wasm_emit_case()`'s ~550-line switch over 49 opcodes averages 11 lines per
opcode. Collapsing the load/store/convert families into opcode tables would
save perhaps 100 lines, but the arms are not as uniform as they look: some use
the `local.tee` peephole and some deliberately use a plain `local.set`, some
emit a trailing `f64.promote`/`f32.demote` and some do not, and several have an
unsigned variant selected from a flag. A table would need six columns of
condition flags, and would bury exactly the details most likely to be a
correctness bug. Left alone deliberately — that is obfuscation, not
simplification.

---

## What was actually fixed

- **Parallel arrays → structs** (`544a43f`). `wasm_emit_function_body()` tracked
  its CFG in eleven `tcc_mallocz`'d `int` arrays indexed in parallel by block
  number, plus two more for the scope stack. Now one `WasmBlock` array and one
  `WasmScope` array: eleven allocations become two, and a new per-block property
  can no longer be added to some arrays but not others.
- **Two write-only arrays deleted.** `blk_term` and `blk_term_flags` were
  allocated, written and freed, and never read.
- **A 19-parameter function signature** (thirteen of them loop-invariant) reduced
  to nine via a `WasmEmitCtx` built once per function.
- **A dead IR field.** `WasmOp.r2` was declared and assigned once, never read.

All verified by output byte-identity: `wasm32-tcc` rebuilds
`emulator/web/rv64.wasm` to the same md5 (`2ffdddc0…`, 30,212 bytes), and the
i386 and riscv64 compilers are byte-identical.

Net line effect: **about +19**. The structures and their comments cost slightly
more than the removed allocations saved. This was a complexity reduction, not a
line-count one, and it is reported that way rather than dressed up.

---

## Honest accounting against the 25,000 target

Available in the backends without either obfuscating the code or losing a
capability: **on the order of 50–150 lines.** The compiler is ~30,700; the gap
to 25,000 is ~5,700.

The backends are not where that comes from. Ranked by what the evidence
actually supports:

| Candidate | Lines | Verdict |
|---|---:|---|
| Stackifier removal | −313 | **No.** Measured 52% emulator slowdown, bigger output |
| Emitter opcode tables | −100 | **No.** Buries per-arm correctness details |
| `gjmp_append` hoist | −14 | Marginal; adds indirection across the arch seam |
| Remaining dead IR/ops | −2 | Done |

The wasm32 build does drag in roughly 2,500 lines of `tccelf.c` it never calls
(`relocate_section`, `build_got_entries`, `create_plt_entry`, `relocate_plt`,
`tcc_add_runtime`, …, all confirmed dead by `--gc-sections`). That is real
structural slack, but it is *shared* code the other two targets need, so it
cannot be deleted from the tree — only excluded from one build. It is worth
knowing about; it is not worth lines.
