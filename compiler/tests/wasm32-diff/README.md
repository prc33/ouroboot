# wasm32 differential testing

Compiles `corpus.c` twice — once with host `gcc` (an independent
implementation, so it can't share a bug with either TCC backend), once with
`wasm32-tcc` — runs both, and diffs the results function-by-function.

```sh
make test
```

## Why this exists

Every wasm-affecting codegen change to this compiler had previously been
verified by checking that `emulator/web/rv64.wasm` rebuilds byte-identical to
a known-good baseline. That's the wrong instrument for a change that's
*supposed* to emit different bytes (a control-flow fix, or eventually a
stack-native rewrite of the backend — see
`docs/wasm-backend-size-2026-08-28.md`), and it only exercises whatever
control-flow shapes `emulator/rv64.c` happens to contain.

This corpus is deliberately weighted toward the two representability gaps
found while investigating that: a `for` loop's rotated layout (condition-test
and increment as two separately-jumped-to positions, but one loop) and
switch-statement case dispatch (every case body emitted before the
compare-and-jump code that reaches it, colliding with loop detection) — plus
`goto`-as-shared-epilogue, switch nested in a loop, loop nested in a switch
case, short-circuit `&&`/`||`, and a switch-heavy "instruction decoder" shaped
like `emulator/rv64.c`'s own `execute()`.

It already found a real bug this way, independent of anything above: a
`WASM_OP_JMP_CMP` whose "taken" edge happens to coincide with the natural
fallthrough (produced by `X && 0`/`X || 1` — an ordinary short-circuit
expression whose other operand folds to a compile-time constant) crashed the
compiler outright, because the fallthrough-elision special case that the
plain `WASM_OP_JMP` handler already had was never extended to
`WASM_OP_JMP_CMP`. Confirmed wasm-specific (not a shared front-end bug) by
checking the same source under TCC's i386 backend + `qemu-i386-static`, which
gets it right — see the fix in `wasm/tccwasm.c` for the full story. This
pattern never occurs in `emulator/rv64.c`, so `rv64.wasm`'s byte-identity was
silent about it; the corpus caught it because it isn't limited to what one
real program happens to contain.

## Adding a test

Every function must be non-static, take no arguments, and return a plain
`int` — `tcc_output_wasm()` auto-exports every non-static top-level function,
and a no-argument `int`-returning signature is what both the generated native
driver (`gen_driver.py`) and the JS harness (`run_wasm.mjs`) can call
generically, with no per-test wiring. Both discover test functions from the
compiled artifact itself (a regex over `corpus.c`'s own declarations, and
wasm module introspection, respectively) rather than a hand-maintained list,
so a new `test_*` function is picked up automatically.
