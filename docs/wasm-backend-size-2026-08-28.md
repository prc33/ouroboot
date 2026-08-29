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

---

## Addendum: the operand-stack model — and deleting everything above

Prompted by: *"does this reduce complexity/line count?"* — answered honestly:
**no.** The three commits above added **+329 lines** to `compiler/wasm/`
(3,174 → 3,553) to buy a 2% smaller module. Each was individually
measured and correct, and each was a *special case* bolted onto the
register machine: `R1_LOCAL`, then `L_LOCAL`, then `VAL_LOCAL`/`VAL_IMM`,
plus a new hook in shared front-end code. Followed by: *"find a way to
completely remove register allocation logic from the wasm code gen, and
reduce complexity there to match the other backends."*

So: **all four flags, both `gen_opi()` special cases, `gen_vstore_hook()`
and its shared-code hook, and the single-slot peephole with its 53-line
`wasm_op_first_input()` lookahead — deleted.** Replaced by one general
mechanism, `WasmVStack` in `tccwasm.c`.

### What it is

The two-pass IR was already the right structure; what was missing was a
model of the thing being emitted onto. `WasmVStack` tracks, during
emission, which registers' values are still sitting on the real wasm
operand stack, in push order. Each arm declares its register operands up
front (`VS_BIND`), and gets told how many are already on top in the right
order; those are taken from the stack, the rest are `local.get`. Results
are recorded, not stored (`VS_DEF` emits nothing). `vs_flush()` parks
everything before any branch and at every block boundary.

The four deleted flags were each an instance of the general property this
computes. `a + b` where both are locals: two `LOAD`s leave two pending
values, the `I32_BIN` finds them adjacent and emits *no operand code at
all* — which is what `L_LOCAL` + `R1_LOCAL` hand-built. `(a+b) + (c+d)`:
the first sum stays on the stack while the second is computed above it,
then the outer add takes both — which no single-slot peephole could ever
see, because it tracks one value and this is two.

### The bug that shaped the design

Pure laziness is wrong, and the corpus caught it within a minute: a value
taken off the stack is gone from the stack *and* was never written to its
local, so a second reader of that register finds nothing. TCC's IR does
emit those — `MOV r1, r0` followed by `r0 = r0 + 1` reads `r0` twice.
First run: `for` loops silently returned 15 instead of 45.

The fix needs no lookahead. An operand is taken for free only when the
consuming op **overwrites that same register anyway** (`r0 = r0 op r1`,
the dominant shape — the old value provably has no later reader).
Otherwise it is taken with a `local.tee`: value stays for this op, local
written for any later one. Still one instruction better than
`local.set` + `local.get`, and certain without any liveness analysis.
That single rule is also why taking *two* operands requires the first to
be the destination — the deeper one isn't on top, so it can't be tee'd on
the way past.

### Measured, on a real build of `emulator/rv64.c`

| | reg-traffic ops | module bytes | `compiler/wasm/` lines |
|---|---:|---:|---:|
| Plain register IR | 5101 | 30,212 | 3,174 |
| + three special-case commits | 4987 | 29,984 | 3,553 |
| **Operand-stack model** | **4552** | **29,113** | **3,360** |

**Against the special cases it replaces: 435 fewer register-traffic ops
(−8.7%), 871 fewer bytes — from 193 fewer lines.** Against the plain
baseline: −549 ops (−10.8%), −1,099 bytes. Register traffic fell from 17%
of emitted opcodes to 15.7%.

Shared front-end intrusion is back to **zero**: `gen_vstore_hook()` is
gone from `tcc.h`, `tccgen.c`, `i386-gen.c` and `riscv64-gen.c`.
`gjmp_hint_loop_range()` remains, which is a control-flow hint, not a
register one.

### What "remove register allocation" does and doesn't mean

Honest accounting. The backend no longer *models* registers: there is no
allocation, no colouring, no spill heuristic, no peephole. What remains
is three one-line functions mapping a register number to a wasm local
index — the spill slot a value goes to when the operand stack can't carry
it, which is what the earlier scoping called for ("spill to a *small*
local pool only when the stack discipline genuinely can't hold").

What could not be removed is upstream: `tccgen.c`'s `get_reg()`/`gv()`
still assign register numbers, because they are shared with i386 and
riscv64 and every `gv()` call site in the front end depends on them. The
backend now largely ignores those numbers — they survive as names for
values, not as a storage model. Removing them outright means the front
end no longer calling `gv()` at all for this target, which is a change to
i386/riscv64's contract, not to `compiler/wasm/`.

Verified: differential corpus 53/53 (nine new cases added for i64/f32/f64,
calls, computed store addresses, and the load-through-the-register-it-
loads-into shape — the arms a control-flow-weighted corpus never reached),
plus the full 8-target regression suite on i386 and riscv64, including
`test-wasm` booting the rebuilt module to `P10 checkpoint OK` through a
live BusyBox shell.

---

## Addendum: the recovery pass wasn't just complex — it was wrong

Prompted by: *"can we now remove the logic that looks for if statements and
loops … I'm hoping the 'fallback' will be sufficient now that we have the
extra info."*

Measured first. Forcing every function onto the dispatch fallback is
**correct but ~57% slower** (60M emulated instructions: 7.60s → 12.0s) and
7.6% larger. The old "52% slower" figure still holds, so "sufficient"
would have been a real regression on the browser demo.

But the measurement turned up something worse. **Every one of the six
functions taking the fallback was the emulator's hot path** — `execute`,
`load`, `store`, `rv_run`, `rv_init`, `flush_tlb` — and five of them were
being rejected over `for (i = 0; i < n; i++)`.

### Two facts the front end had and wasn't passing on

**1. A loop's continue target.** tccgen.c emits a for-loop rotated:
increment *before* body, jump over it on entry. So the body's exit edge
and every `continue` run backwards by position into the increment. The
guard accepted a backward edge only when it targeted a loop *header*, and
could not tell this apart from a jump into an already-closed scope. It
rejected both — and with them the whole function. `flush_tlb()` is four
lines long and was compiled the slow way.

`gjmp_hint_loop_range()` now carries `cont`, which tccgen.c already had
(it is the `d` that `gjmp_addr`/`gsym_addr` use). The backend undoes the
rotation: emit `[condition][body][increment]`, and the backward edge
becomes a fallthrough while the increment's becomes the one repeat edge.

**2. A switch's layout.** tccgen.c emits `[jump to lookup][bodies][lookup]
[break]`, because `gcase()` can only build the comparison chain once every
case value is known. So every edge selecting a case runs backwards into a
closed scope — not expressible in structured control flow at all. New
`gjmp_hint_switch_range()` says which region is which; the backend swaps
them, and each becomes a forward branch out of a block scope, which is
exactly the nested-blocks shape a switch should compile to.

Both are pure block-reordering: no op moves, ranges stay contiguous, and
inner constructs travel as one piece (hints arrive innermost-first).

### A latent bug this exposed

Opening one scope per case target hit fixed 64-entry arrays that silently
dropped everything past the 64th — not a missed optimization but a `br`
with nowhere to land. Now sized to the block count, and pinned by a new
80-case corpus test that fails with `no scope for JMP target block 195`
against the old limit.

### Measured, `emulator/rv64.c`, 60M instructions

| | fallbacks | time | module |
|---|---:|---:|---:|
| Before | 6 of 33 | 7.60s | 29,113 B |
| + loop continue target | 1 of 33 | 5.86s | 28,529 B |
| + switch layout | **0 of 33** | **4.17s** | **24,190 B** |

**45% faster and 17% smaller**, and the dispatch fallback is now
unreachable for this program — the 373-block instruction decoder called
~294.6M times per boot is structured code.

### On the original question

The recovery pass did not shrink, and deleting it was the wrong move: it
pays for itself several times over. What was actually wrong is that it was
*inferring* two things the parser knew for certain and threw away, and
guessing wrong on the commonest loop in C. That is the same lesson as
`gjmp_hint_loop_range` itself, twice more — and it is why the fallback
still exists, since `goto` can still produce control flow no hint makes
structured.

Verified: differential corpus 57/57 (four new switch stress cases), full
8-target regression on i386 and riscv64 including `test-wasm`.
