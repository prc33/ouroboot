# Why the wasm backend is 3,174 lines — and why the hints were the wrong half

Prompted by: *"I'm still very confused by the size of the wasm backend. The
generic TCC code knows basic block boundaries and all the things wasm is
fighting to work out. Maybe wasm should codegen directly from the AST rather
than trying to use the generic register machine codegen at all — the hints
might be fighting a losing battle."*

**That's right, and the measurements say it more strongly than the question
does.** The control-flow recovery I've spent two commits improving is only
**20%** of the backend. The larger cost — **54%** — is machinery that exists
purely to model *registers that wasm does not have*, and then to partially
undo that modelling again. Hints cannot touch any of it.

---

## Where the 3,174 lines actually go

| Lines | % | What |
|---:|---:|---|
| ~1,700 | 54% | **Register-machine impedance**: inventing registers, then translating them back to a stack |
| ~621 | 20% | **Control-flow recovery**: rediscovering loop/branch structure from flattened jumps |
| ~850 | 27% | Module writing, symbol/reloc resolution, LEB128, opcode tables — irreducible |

### The 54%: modelling registers wasm doesn't have

`wasm-gen.c` declares twelve fake registers to satisfy TCC's backend
interface:

```c
TREG_I0 = 0, TREG_I1, TREG_I2, TREG_I3,   /* "integer registers" */
TREG_L0, TREG_L1, TREG_L2, TREG_L3,       /* "long registers"    */
TREG_F0, TREG_F1, TREG_F2, TREG_F3,       /* "float registers"   */
```

None of these exist. Each is mapped onto a wasm *local* by helpers
(`wasm_i32_reg_local`, `wasm_i64_reg_local`, `wasm_f64_reg_local`) called
**82 times** across the emitter. `wasm_emit_case()`'s non-branch arms — **489
lines** — are almost entirely "take operands out of fake registers, push them
on the real wasm stack, do the operation, put the result back in a fake
register."

Then, because that round-trip is pure waste, the emitter carries a *second*
layer to partially undo it: the `WB_SET_OR_TEE` / `WB_GET_OR_SKIP` peepholes
(**58 uses**) plus a 53-line lookahead helper (`wasm_op_first_input`), whose
entire job is to notice "this value is already on the wasm stack, don't
round-trip it through a local."

**The pipeline is losing information twice:**

```
tccgen.c's vstack  →  gv() forces a register  →  wasm local  →  peephole  →  wasm stack
   (a stack!)            (loses stack order)      (spill)     (partial undo)   (a stack!)
```

TCC's front end *already has a value stack*. wasm *is* a stack machine. The
register machine is an artefact sitting between two stack machines.

### The cost is in the output too, not just the source

Instrumenting the emitter over a real build of `emulator/rv64.c`:

```
WASM LOCALS: get=2751 set=1937 tee=413 = 5101 register-traffic ops
             of 29974 emitted opcodes (17%)
```

**17% of every opcode emitted into `rv64.wasm` is `local.get`/`local.set`/
`local.tee` shuffling values through fake registers** — and that is the number
*after* the peepholes have already removed everything they can. The browser
executes that overhead 294.6 million times per boot test.

### The 20%: control-flow recovery (what the hints addressed)

Inside `wasm_emit_function_body()` (605 lines), only 74 are real prologue work:

| Lines | Region |
|---:|---|
| 74 | prologue / locals / param spill |
| 44 | basic-block partitioning |
| 116 | successor + loop + reducibility analysis |
| 111 | forward-scope placement + nesting fixpoint |
| 189 | structured emission |
| 71 | `br_table` dispatch fallback |

plus ~90 lines of branch arms in `wasm_emit_case()`. **~621 lines rediscovering
what `tccgen.c` knew for certain and threw away.**

---

## "Codegen from the AST": the one correction

**TCC has no AST.** It is strictly single-pass: `block()` (tccgen.c:5732)
parses and emits in the same traversal, and nothing is retained. So
"generate from the AST" is not literally available.

But the *intent* is exactly right, and there is a realizable form of it:
**generate from the parser**. At the moment `block()` is handling `TOK_WHILE`,
the structure is not merely inferable, it is *the program state*. The problem
is only that the current backend interface offers nowhere to say so, so the
structure is flattened into patchable jumps and then painstakingly
reconstructed downstream.

That is also the honest verdict on my last two commits: **they were a local
optimum on the smaller half of the problem.** They bought real things — a
latent crash fixed, the nested-loop fixpoint deleted outright — but they were
feeding better information into an architecture whose main cost is elsewhere.
"Fighting a losing battle" is fair for the 54%; the hints simply cannot reach
it, because no amount of control-flow metadata changes the fact that `gv()`
has already forced a value into a register before the backend sees it.

---

## What a stack-native wasm backend would look like

Two independent changes, in increasing order of difficulty:

**1. Structural emission for control flow.** Hook the parser rather than the
jump stream: `wasm_begin_loop()` / `wasm_end_loop()` / `wasm_begin_if()` /
`wasm_else()` / `wasm_end_if()` / `wasm_begin_switch()` / … C's structured
constructs map onto wasm's `block`/`loop`/`if`/`br`/`br_table` essentially
one-to-one, which is why this is worth doing: the recovery pass exists only
because that correspondence was discarded. `break`/`continue` become `br` to a
tracked scope depth — TCC already threads these through
`cur_scope->bsym`/`csym`. Eliminates most of the 621 lines *and* both fallback
paths, and dissolves the for-loop-rotation and switch-dispatch problems rather
than working around them, because emission follows the source's own nesting
instead of the flattened layout.

**2. Stack-first expression emission.** Stop materialising into twelve fake
registers; keep values on the wasm operand stack, and spill to a *small* local
pool only when the stack discipline genuinely can't hold. This is the 54%, and
it is the harder half, because TCC's front end does shuffle its value stack:
`vswap` (33 call sites), `vdup` (17), `vrott` (6), `save_reg` (4). Those are
real — an early `--gc-sections` run appeared to show them unreachable in the
wasm32 build, but that was **gcc inlining them**, not disuse; checking the
actual call sites corrected it. So spill locals remain necessary. The point is
that they'd be an occasional fallback rather than the mandatory path for every
single value.

Prize: plausibly **~3,174 → ~1,600 lines**, *and* the 17% opcode overhead goes
away (a faster browser demo, not just a smaller compiler), *and* the fallback
dispatch path — with its 52%-slower output — stops being reachable at all.

---

## Why this is not in this commit

This is a rewrite of `wasm-gen.c` and `wasm_emit_case()`, not an incremental
change, and the current safety net cannot support it. Everything verified so
far has leaned on **byte-identical `rv64.wasm`** — which is precisely the wrong
instrument here, because a stack-native backend is *supposed* to emit
different, better code. The moment that check stops applying, the only
remaining evidence is one boot smoke test, and the failure mode of a
mis-emitted wasm module is silent miscompilation, not a build error.

**The prerequisite is a real wasm test corpus** — `compiler/tests/wasm32/
examples.c` is 67 lines. What's needed is either substantially broader coverage
with known-good expected outputs, or differential testing: compile the same C
with the i386/riscv64 backends and the wasm backend, run both, compare results.
That is independently valuable (it would also retire the "we can't safely fix
the for-loop rotation" blocker from the previous document) and is the honest
next step before touching codegen.

**Done**: `compiler/tests/wasm32-diff/` (differential against host gcc, 29
functions weighted toward exactly the shapes this document and the previous
one identified). It found a real bug on first run — a degenerate
`WASM_OP_JMP_CMP` (both branches landing on the same block, produced by an
ordinary `X && 0`/`X || 1` short-circuit) crashed the compiler outright,
independent of anything about loops or switches, confirmed wasm-specific
against i386 + `qemu-i386-static` before fixing. `rv64.wasm` stayed
byte-identical throughout — this pattern doesn't occur in `emulator/rv64.c`,
which is exactly why byte-identity was silent about a crash-on-sight bug. See
the commit and `compiler/tests/wasm32-diff/README.md` for the full story. The
corpus is the safety net (1) and (2) below still need before either is safe
to attempt.

## Recommendation

1. ~~Build the wasm test corpus / differential harness first.~~ **Done** —
   `compiler/tests/wasm32-diff/`.
2. **Then structural emission (1)** — self-contained, deletes the most code
   per unit of risk, and removes the two known-bad fallback paths.
3. **Then stack-first expressions (2)** — the big prize, both for size and for
   browser performance.
4. **Keep the existing hints in the meantime.** They're inert for
   i386/riscv64, they fixed a real crash, and under (1) they become the
   natural place the structural events attach — not wasted work, just
   insufficient on their own.

Worth stating plainly: the two hint commits made things measurably better and
provably safe, but they were treating the smaller half. The question was the
right one to ask.

---

## Addendum: a peephole extension, tried, measured, reverted

Direct follow-up once the corpus existed: does the corpus let (2) actually
happen, and how much does it save? Traced the exact mechanism first, then
attempted the smallest real slice of it, and it taught something the
hand-analysis alone didn't predict.

**The two-pass IR isn't the obstacle here.** `load()` (called by the shared
`gv()`) still just records a symbolic `WasmOp` — it never decides `local.set`
vs `local.tee`. That decision is made entirely at emission time, by
`wasm_op_first_input()` and the `WB_SET_OR_TEE`/`WB_GET_OR_SKIP` peephole. So
improving stack residency for the common case doesn't need to touch
`gv()`/`gv2()`/`load()`/`tccgen.c` at all — good news for scope.

**Why the 17% survives despite an existing peephole.** `gv2()` (`tccgen.c`)
always materializes the left operand before the right, for same-class
operands — the only kind `wasm-gen.c` ever requests. But
`wasm_op_first_input()` reports `r0` (the left/dst operand, computed *first*,
two ops before the combine) as what the combine wants, while `r1` (the right
operand, computed *immediately* before the combine) is the one actually still
sitting on the wasm stack from its own `local.tee`. That looks like a
straightforward miswiring: the peephole can only ever match the operand that
was computed further away, not the one that's actually fresh.

**Fix attempted:** for the five commutative i32 ops (`+ * & | ^`, where popped
operand order provably can't change the result), swap which operand
`wasm_op_first_input()` prefers to `r1`, and reorder the combine's own
emission to match. Verified *correct* immediately — the differential corpus
passes 29/29, confirming the reordering is semantically sound.

**Verified *not an improvement*, by direct measurement, not assumption.**
Instrumented the peephole's own hit/miss counter on a real build of
`emulator/rv64.c`:

| | Peephole hits | Misses |
|---|---:|---:|
| Baseline (`r0` priority) | 390 | 291 |
| Changed (`r1` priority for commutative ops) | 347 | 334 |

**43 fewer hits, exactly matching the +43 opcodes measured separately.** The
hypothesis was wrong: `r0` is hit *more* often under the existing rule than
`r1` ever would be under the new one. The likely reason, not confirmed further
than this: real code's dominant win isn't "this one combine's freshly-computed
right operand" — it's accumulator-style chaining (`sum += x` in a loop, or one
statement's result feeding the next statement's left operand), where `r0`
carries a value *into* a later combine across more than one op, and the
existing rule already captures that. The peephole extension traded a bigger,
real pattern for a smaller one and came out behind. **Reverted** — a measured
regression doesn't ship because it happens to also be correct.

This sharpens the earlier scoping rather than contradicting it. A single-slot
"what's on top of the stack right now" tracker is provably too narrow to
capture both patterns (chained `r0` *and* fresh `r1`) at once — that needs
genuine multi-value / stack-depth-aware tracking, which is a materially bigger
change than a peephole table edit. It is, in other words, exactly the
stack-first rewrite (2) already scoped above, not a shortcut around it. No
code changes shipped from this addendum; the corpus is what made "tried it,
it's a regression, proved it in twenty minutes instead of finding out from a
production slowdown" possible at all.

---

## Addendum: preventing the register instead of recovering it

The peephole addendum above tries to *recover* stack residency after the
fact, competing for one slot. The alternative — *prevent* the register from
ever being allocated, at IR-recording time, for operands that are provably
"immediately consumed, nothing intervening" — doesn't have that competition
problem, because it doesn't touch the peephole at all. `gen_opi()` is
`wasm-gen.c`'s own code; it doesn't have to call `gv2()` — it can inspect
`vtop[-1]`/`vtop`'s raw `SValue` fields directly and skip register
materialization for a plain, non-volatile `int` local
(`wasm_is_simple_local()`).

**Two commits shipped from this:** first, the right operand of `+ - & ^ | *
<< >> ...` and comparisons, when it's a simple local, is loaded inline from
its own frame slot instead of round-tripped through a register
(`WASM_OP_FLAG_R1_LOCAL`). Second, when *both* operands are simple locals,
neither touches a register at all — for comparisons this means zero
registers used, since the result always lands in the fixed `local_cmp` slot
already; for arithmetic, `get_reg()` (not `gv()`) grabs a destination purely
to hold the result, never loaded into (`WASM_OP_FLAG_L_LOCAL`). Verified with
9 new operand-order-sensitive corpus tests (non-commutative ops are
load-bearing here — a swapped operand flips the answer, not just the cost).
Measured on `emulator/rv64.c`: 5101 → 5047 → 5033 register-traffic ops;
30,212 → 30,104 → 30,076 bytes.

**Diagnostic breakdown of what was left**, by `WasmOp` kind: `LOAD_I32`
(885), `LOAD_I64` (504), `STORE_I32` (445), `STORE_I64` (411), `JMP_CMP`
(402, mostly reading the fixed `local_cmp` slot — not real waste), `I32_BIN`
(343), `I64_BIN` (327, untouched), `SET_CMP_I32` (322), `CALL` (251,
inherent to wasm's calling convention), `I32_CONST` (196), `JMP` (186),
`SET_CMP_I64` (161). Loads and stores dominate, not binary-op operands —
reframing where the remaining opportunity actually is.

**Why the same trick can't reach `store()` from inside `wasm-gen.c` alone.**
`vstore()` (`tccgen.c`, shared by every assignment TCC compiles, for all
three backends) unconditionally calls `gv(RC_TYPE(dbt))` to materialize the
source value *before* ever invoking the backend's own `store()`. Unlike
`gen_opi()`, `store()` has no opportunity to intervene — the front end has
already decided by the time backend code runs.

**A peephole-recovery approach for `store()`'s source was also considered,
and ruled out as actually unsafe, not just suboptimal**, before writing any
code. wasm's `i32.store` requires `[address, value]` stack order; the
store's own address computation (`wasm_emit_addr`) always emits bytes
*between* any preceding "hot" value and where the store needs to read it —
any attempt to keep a preceding value on the stack for a store to consume
would corrupt operand order, risking silent data corruption rather than a
crash. Caught by tracing the emission order, not by the corpus.

**The fix: a new hook, `gen_vstore_hook()`, called from `vstore()` itself
right before its `gv()` call** (`compiler/tcc.h` declares it; i386/riscv64
stub it to `return 0`, the established no-op precedent from
`gjmp_hint_loop_range()`). This runs *before* the front end has committed to
materializing anything, while `vtop`/`vtop[-1]` are still exactly what the
parser produced — the same position `gen_opi()` already had, just for
assignment instead of binary ops. Scoped to plain `int`-to-`int` stores
only (sidesteps cast interaction, bitfields, structs, two-word types, which
`vstore()` already routes elsewhere before reaching the hook's call site):
if the destination is a simple local and the source is either a simple
local or a compile-time constant, the backend emits the complete store
inline — address, then value (local-load or const), then the store
instruction — and returns 1, and `vstore()` skips its own `gv()`+`store()`
entirely, falling straight through to its unchanged trailing
`vswap();vtop--;`. The "leave `vtop` untouched" contract is what makes this
safe for chained/nested assignment (`a = b = c`, or `(a = b) + 1`): the
unmodified source `SValue` is still a fully valid representation of "the
assignment expression's result" for whatever consumes it next.

Verified with 6 new corpus tests (local-to-local, const-to-local,
self-assignment, `a = b = c` chaining, the assignment expression's own
result consumed immediately, and repeated assignment inside a loop) — 44/44
still pass. Measured on `emulator/rv64.c`: 5033 → 4987 register-traffic ops
(46 fewer); 30,076 → 29,984 bytes (92 bytes smaller).

Smaller win in absolute terms than the two `gen_opi()` commits, because
`STORE_I32`'s source is very often already the *result* of a computation
(not a bare local or constant) — this hook only fires for the subset of
assignments whose right-hand side is literally a name or a literal. Real,
verified, and — more importantly — it establishes the hook itself as a
reusable interception point: the same `vtop`-still-untouched contract is
available to any future backend work that needs to see an assignment before
the front end forces it into a register, not just this one narrow case.
