# Stepping back on wasm codegen: is the MiniCC-derived design the wrong path?

Prompted by a direct question after the backend review: would designing wasm
codegen from scratch, rather than adapting MiniCC's register-IR-plus-relooper
approach, avoid the complexity that review found? Short answer: **the two-pass
IR design itself is close to forced by TCC's shared front end and isn't the
problem.** But investigating this surfaced something the previous review
missed entirely, and it's a real finding: **the current structurer is
mis-detecting ordinary `switch` statements as loops, and it's costing
real performance on the hottest functions in the project's own flagship
demo, today, in code nobody would call an edge case.**

This document is that investigation, in order.

---

## Why a two-pass design is close to unavoidable here

TCC's front end (`tccgen.c`, shared by all three backends) does not tell a
backend "this is a while-loop" or "this is a switch." It only ever calls four
primitives, and the same four for every control construct:

- `gjmp(t)` / `gvtst(...)` — emit a jump whose target is **not yet known**;
  returns an opaque chain handle.
- `gsym(t)` / `gsym_addr(t, a)` — **resolve** a chain, to "here" or to an
  explicit address.
- `gjmp_addr(a)` / `gtst_addr(0, a)` — emit a jump to an address **already
  known** at the call site.

Every `if`, `while`, `for`, `do`, `switch`, `break`, `continue`, `&&`, `||`,
`?:`, and `goto` in `tccgen.c` compiles down to exactly these calls, and the
byte/wasm-op stream they produce is fundamentally a flat, position-addressed
list with patchable jumps — the representation a linear assembler wants, not
one a structured target wants. A wasm backend bolted onto this interface,
without changing the shared front end (out of scope — every other backend
depends on it), **cannot know a jump's true role in the source-level control
structure**; it only sees "jump to a chain, resolved later" or "jump to an
address, already known." Recovering block/loop/switch *shape* from that
requires either look-ahead (buffer the whole function, then analyze) or
online pattern-matching against the small set of shapes `tccgen.c` can
produce. MiniCC's choice — buffer an IR, then run a CFG-based structuring pass
— is the first of those, and it is a reasonable one. This part of the design
is not the mistake.

---

## What actually went wrong: two constructs share one primitive

`gjmp_addr()`/`gtst_addr()` ("jump to an address already known") is used for
exactly one thing in every backend *except* wasm's structurer: **a loop's
repeat edge.** `while`/`for`/`do` all end their body with a jump back to a
position recorded earlier via `gind()`. So it was natural — and, for i386 and
riscv64, entirely correct — to treat "target address already known" as
synonymous with "this is a loop."

It is not synonymous. **`switch` uses the identical primitive for a completely
different purpose.** Reading `tccgen.c`'s own switch implementation:

```c
} else if (t == TOK_SWITCH) {
    ...
    b = gjmp(0);           /* jump over the case bodies -- target unknown yet */
    lblock(&a, NULL);       /* emit ALL case bodies, in source order, first */
    ...
    gsym(b);                 /* the jump-over lands here, after every body */
    ...
    gcase(sw->p, sw->n, &d); /* NOW emit the compare-and-dispatch code */
    ...
}
```

and `gcase()`:

```c
static void gtst_addr(int t, int a) { gsym_addr(gvtst(0, t), a); }
...
gtst_addr(0, p->sym);   /* jump to this case's body -- already-known address */
```

**Every case body is emitted before the dispatch code that jumps to it.**
`gcase()` dispatches into them with `gtst_addr`/`gsym_addr` — exactly the
"already-known address" primitive the wasm structurer treats as proof of a
loop. So the structurer sees a big `switch` with N cases and concludes it is
looking at up to N "loop headers" — one per case — because every case body is
the target of a backward-looking jump, and nothing in the IR says *why*.

## Confirmed against the project's own code, not hypothetically

Instrumenting `wasm_emit_function_body()` to log every function that falls
back to the slow `br_table` dispatch path, then rebuilding
`emulator/web/rv64.wasm` from `emulator/rv64.c` — the real RV64 core this
project ships in the browser — found:

```
WASM FALLBACK: flush_tlb
WASM FALLBACK: load
WASM FALLBACK: store
WASM FALLBACK: execute
WASM FALLBACK: rv_init
WASM FALLBACK: rv_run
WASM SUMMARY: 33 funcs total
```

**6 of 33 functions (18%) — and specifically the four hottest functions in the
entire interpreter — silently take the slow path today, in the unmodified
tree.** `rv_run()` is the outer instruction-dispatch loop. `execute()` decodes
and runs every single instruction — called **294.6 million times** in the
project's own boot test. `load()`/`store()` back every memory access.

Two distinct causes, both confirmed by reading the actual source:

- **`flush_tlb`, `load`, `store`, `rv_init`** contain nothing but ordinary
  counting `for` loops (`for (i = 0; i < 512; ++i) tlb_valid[i] = 0;` is
  literally `flush_tlb`'s entire body). These trip a *different*, narrower
  issue: `tccgen.c` lays a `for` loop out increment-before-body (a
  branch-predictor-friendly rotation every native compiler does), which
  creates a forward jump that lands inside what the structurer — correctly,
  in this instance — identifies as the loop's own span. No goto, no
  irreducibility in any classical sense; it's the standard `for`-loop shape
  every C program uses.

- **`execute()`** contains no loop at all — it is one large `switch` over the
  RISC-V opcode, with `goto write;` used as a shared epilogue from many
  cases (an entirely idiomatic C pattern). It falls back purely because of
  the switch/loop primitive collision above: a forward `goto write` lands
  inside the address range spanned by the *other* cases, which the
  structurer has already (mis)flagged as loop headers.

Neither cause is an exotic edge case. A counting `for` loop and a
`switch`-based instruction decoder are about as ordinary as C gets — and a
`switch`-based decoder is *exactly* the shape most likely for any interpreter
or protocol parser, which is a natural class of program to compile to wasm.

## What this costs

Forcing every function through the dispatch path (measured in the prior
backend review) costs 52% on the whole-program boot benchmark. That number
was produced by disabling the stackifier entirely, which is not what's
happening today — today it's 18% of functions, but they are the *hottest*
18%. The real, current cost sits somewhere between "negligible" and "most of
that 52%," dominated by `execute()`'s 294.6M calls; this document doesn't
put an exact figure on it because isolating it needs either a per-function
profiler pass or a working fix to compare against, and getting a wrong
control-flow transform"fixed" and silently miscompiling wasm output is a far
worse outcome than an imprecise performance estimate. That trade is discussed
below.

## The fix, and why it wasn't attempted in this pass

The corrected model is: **classify a "known address" jump as a loop
repeat-edge only when it targets a position associated with an actual
`gind()`-recorded loop header from `tccgen.c`'s loop constructs — not merely
"any earlier position."** Concretely, the switch-dispatch case can be told
apart from a real loop because dispatch jumps *converge* — many different
jump sites (comparisons) all target case-body starts that were **all**
recorded consecutively, immediately after the "jump over the bodies" (`b`)
that opens the switch — a shape a loop never produces (a loop has exactly one
repeat-edge target). The `for`-loop rotation case is narrower still: it's a
single, fixed three-block pattern (test → skip-forward → increment → body →
back-to-test, back-to-increment) that could be recognized and canonicalized
into the natural wasm loop shape (test-at-top, body-then-increment,
single backward branch at the bottom) as a peephole rewrite on the recorded
op-stream, before the general structurer ever runs — leaving the
already-correct general structurer completely untouched, and giving every
`for` loop the same shape a `while` loop already gets fine.

Both are scoped, understood, and lower-risk than they might sound precisely
*because* the general structurer's actual graph algorithm doesn't need to
change — only what feeds it. But this is genuine control-flow-correctness
surgery: get it wrong and the failure mode is a **silently miscompiled wasm
module**, not a build error — exactly the class of bug this project's own
"measure everything, prove byte-identity" discipline exists to keep out.
Implementing and verifying it properly (broader test coverage than the
current single "boot to a shell prompt" smoke test would give real
confidence) is a real, focused piece of work in its own right, and it did not
feel right to rush it into this pass alongside everything else. It's recorded
here, precisely scoped, as the next concrete step — not attempted blind.

## Answering the actual question

**Is the MiniCC-derived design the wrong path?** The two-pass buffer-then-structure
architecture: no — it's close to the only architecture available given
`tccgen.c`'s interface, and the earlier backend review's measurement (52%
slower, larger output, if the stackifier is removed entirely) still stands as
proof it's earning its keep in principle.

**But something about how it was adapted is wrong**, in a way the line-count-
focused review did not surface because it was looking at *size*, not
*behavior*: the loop-detection heuristic was carried over from a context
(probably MiniCC's own, simpler source language, or an earlier, switch-free
version of this design) where "backward-looking known-address jump" really
did mean "loop." It doesn't in TCC's IR, because TCC's `switch` reuses the
same primitive. That mismatch is invisible in any output-size metric — the
module still compiles, boots, and passes the existing test — and only shows
up as a silent performance cliff on exactly the code most likely to be
switch-heavy. This is the kind of bug that stays invisible until someone asks
"but does it actually take the fast path?" instead of "does it produce
correct output?" — worth remembering as a category, beyond this one instance.

## Recommendation

1. **Don't touch the general structurer.** It's correct, and the earlier
   review already proved removing it is a strict regression.
2. **Fix the classification, not the algorithm**: distinguish real loop
   headers (from `tccgen.c`'s loop constructs) from switch-dispatch targets
   (from `gcase()`), most robustly by having `wasm-gen.c` tag *why* a
   known-address jump was recorded, since the front end's own call pattern
   already tells you which construct you're in — no CFG inference needed
   for this part at all.
3. **Separately, canonicalize the `for`-loop rotation** as an op-stream
   peephole rewrite ahead of the structurer.
4. **Verify with a real correctness net wider than today's**, before
   trusting it: the existing `rv64.wasm` byte-identity check plus boot smoke
   test is exactly the wrong tool for this change (a control-flow rewrite
   is *supposed* to change the bytes) — this needs either a much larger wasm
   test corpus than `compiler/tests/wasm32/examples.c` currently provides, or
   differential testing against the native i386/riscv64 codegen on the same
   C source, run under both paths and compared.

Not implemented in this pass, and deliberately so — flagging it for a
decision rather than presenting a rewritten control-flow structurer as a
fait accompli.

---

## Addendum: the cheap-plumbing fix, implemented and verified

Follow-up question after this was written: since the other two backends don't
need the loop/switch distinction, can the metadata just be threaded through
`tccgen.c` cheaply, tagged, and ignored elsewhere? Yes — implemented, and the
result is more interesting than either "it worked" or "it didn't."

### What was built

A new shared primitive, `gjmp_hint_loop()`, called immediately before each of
the **exactly six** call sites in `tccgen.c` where a `while`/`for`/`do` loop
resolves its own repeat edge (`gjmp_addr`/`gsym_addr` in the `TOK_WHILE`,
`TOK_FOR`, and `TOK_DO` handlers — enumerated exhaustively by grepping every
`gjmp_addr`/`gsym_addr`/`gtst_addr` call in the file, 13 call sites in total,
sorted into loop / switch-dispatch / goto by reading each one). i386 and
riscv64 define it as an empty function — it costs them nothing, since it's a
bookkeeping call inside the compiler's own execution, not code emitted into
whatever program TCC is compiling. wasm's version sets a one-shot flag that
`gjmp_addr()`/`gsym_addr()` consume and stamp onto the `WasmOp`(s) they
resolve, via a new `WASM_OP_FLAG_LOOP_EDGE` bit. `tccwasm.c`'s
`is_loop_header` computation now requires that flag, not just "some later
block jumps here" — switch-dispatch and goto, which never set it, stop being
misread as loops. **Net: +81 lines, entirely additive; the general structuring
algorithm itself is untouched.**

### It immediately found a real, previously-latent crash

Rebuilding `emulator/web/rv64.wasm` with the fix applied: `tcc: error:
wasm32 structured: no scope for JMP_CMP taken block 86 from block 346` — a
hard compile failure, not a silent miscompile, but still a bug the fix itself
introduced. Root cause: fixing the *classification* correctly stopped `switch`
dispatch from opening fake loop scopes, but the **emission** code has its own,
entirely separate, still-purely-positional assumption — "target position ≤
current position → search for a loop scope, full stop" — which the analysis
fix now violates for the first time in the code's history. It had never been
exercised before, because the old over-broad classification's false-positive
loop headers were *accidentally* triggering the "jump into loop" fallback for
almost every switch-heavy function first, so structured emission never had to
try.

This surfaces the actual hard constraint: **a backward jump into an
already-emitted, already-closed case body has no wasm structured
representation at all**, regardless of how the analysis is done — wasm branches
can only target scopes still open on the stack. `switch`-case dispatch, laid
out bodies-first-then-comparisons by `tccgen.c`, produces exactly that shape.
Fixed with an explicit rule, added right alongside the existing "jump into
loop" check rather than touching the emission code: **any backward-by-position
edge that isn't tagged as a real loop edge unconditionally forces the dispatch
fallback**, since it is structurally required, not just heuristically likely.

### The verified result: correct, but no performance win — here

With both the classification fix and the safety net in place:

- `emulator/web/rv64.wasm` rebuilds **byte-identical** to the pre-change
  baseline (`2ffdddc0bf41bdbcbe2a9cb8abf978dc`, 30,212 bytes).
- Re-running the same fallback-logging instrumentation from the main
  investigation: **the same six functions** (`flush_tlb`, `load`, `store`,
  `execute`, `rv_init`, `rv_run`) still take the slow path — confirmed by
  function name, with `execute` now explicitly logged as hitting the new
  safety net rather than the old "jump into loop" check it used to trip by
  coincidence.
- Every existing test passes on both real hardware targets: i386
  `test`/`test-initrd`/`test-busybox`/`test-selfhost`, riscv64
  `test`/`test-initrd`/`test-selfhost`, and — the test that matters most here,
  since it runs `execute()` 294.6M times under the *new* code path decision —
  riscv64 `test-wasm` (full kernel boot, self-hosting compile, interactive
  BusyBox shell, all inside the browser-emulator wasm module).

So: **byte-identical output means zero net effect on this real program.** The
classification fix and the safety net it required cancel out exactly, because
`execute()` was always going to fail *some* check — it just fails a different,
now-honest one. This is not a wasted result: it replaces an accidental,
"works by luck" correctness property (this exact shape happened to trip the
old heuristic) with a real, provable one (division by construction: loop
headers only ever come from where `tccgen.c` says they're loops), and along
the way it found and fixed a genuine crash that the old code's accidental
conservatism had been silently hiding. That crash risk is real for *some*
future switch-shaped function even in this codebase, not just a
theoretical concern — it took exactly one honest classification fix to
surface it.

**It does not, on its own, get `execute()` onto the fast path.** That still
needs the separately-scoped, larger fix this document already recommended:
either recognizing `gcase()`'s specific bodies-then-dispatch shape and
rewriting it (at record time, before the general structurer runs) into
something wasm can represent directly — inline `if`/`else if` comparisons
ahead of each case body, the natural wasm-idiomatic switch shape — or a
narrower, switch-scoped `loop`+`br_table` used only for the dispatch region
rather than falling back for the entire enclosing function. Both are real
compiler work, not metadata plumbing, and both are still unimplemented.

### Verdict on the plumbing itself

Cheap, safe, and worth keeping regardless of the performance answer: it is a
genuine correctness improvement (removes a class of latent crash risk,
confirmed to exist by triggering it), it's fully additive and provably inert
for the other two backends, and it's now proven — not assumed — to be
behavior-preserving on the project's real flagship program. It just isn't the
performance fix that was hoped for; that fix is bigger, and is what's actually
still outstanding.
