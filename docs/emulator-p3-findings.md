# Emulator P3: kernel.elf booting in an actual browser tab

`docs/emulator-plan.md`'s P3 exit criterion: `kernel/kernel.elf`'s full
checkpoint output visible in a real browser tab, not just Node's
stdout. Reached and verified against a real headless Chromium (via
Puppeteer -- not just eyeballed, and not just simulated in Node; see
below for why that distinction mattered).

## What's implemented

`emulator/js/index.html` + `app.js` (main thread: xterm.js terminal,
`fetch()`s `kernel.elf`, wires `postMessage` to a Worker) +
`worker.js` (the actual CPU loop, off the main thread so it never
blocks the terminal) + `browser-shim.js` (a small CommonJS-in-the-
browser loader). `memory.js`/`csr.js`/`mmu.js`/`uart.js`/`cpu.js`/
`elf.js` -- the exact same files Node's `boot.js` and
`kernel/Makefile`'s `test-js` target already exercise -- are loaded
completely unmodified.

## Two real bugs, both found only by testing in a *real* browser

Node happily runs `require()`-based CommonJS correctly by
construction (each file gets its own module scope automatically), so
neither of these could have been caught by more Node testing, no
matter how thorough -- they're specific to how a browser (or a Worker)
actually loads and scopes script files, which is a genuinely different
execution model. Worth stating plainly: P1/P2's Node-only testing was
real and valuable, but it could not have found either of these; only
booting under an actual browser engine could.

**1. `importScripts()` shares one global scope across every file.**
First attempt loaded each core file via plain `importScripts('memory.js')`
etc, then a `require()` shim that just looked up an already-populated
registry. This looked completely fine and matched every mental model
of "just load the files in dependency order" -- until real multi-file
loading was tested under Puppeteer and threw `Identifier 'CSR' has
already been declared`. Root cause: `cpu.js`'s own top-level
`const { CSR, ... } = require('./csr')` is a perfectly ordinary,
module-private destructuring assignment under Node (each file has its
own closure) -- but `importScripts()` executes every file directly in
the Worker's single shared global scope, with zero per-file isolation,
so that "local-looking" declaration becomes a *second* global `const
CSR` binding, colliding with `csr.js`'s own top-level `const CSR =
{...}`. First fix attempt (guard against the whole worker script
re-running) was based on a wrong diagnosis -- traced with explicit
`console.log` calls between each `importScripts()` call (captured via
Puppeteer's `page.on('console')`, since Worker console output *is*
forwarded to the page) before finding the real cause. Real fix:
`browser-shim.js`'s `loadCjsModule()` fetches each file's source as
text (synchronous XHR -- deprecated on the main thread but still the
right tool inside a Worker, which has no top-level `await`) and runs
it via `new Function('module', 'exports', 'require', source)`,
exactly reproducing Node's own per-file module-wrapper technique.

**2. Testing against a stale `kernel.elf`.** After the scope-isolation
fix, the browser test failed with `not ELFCLASS64
(e_ident[EI_CLASS]=1)` -- the *previous* `kernel/kernel.elf` on disk
was still i386's (from an unrelated regression check earlier the same
session that ran `make clean && make test` without `ARCH=riscv64`).
Not an emulator bug at all, but real enough to be worth recording:
`kernel/kernel.elf` is a build artifact shared across both `ARCH=`
targets under the same filename (the same gotcha
`kernel/Makefile`'s own top comment already documents for `clean`),
so any workflow that fetches it needs the right one actually built
first. `make ARCH=riscv64` before testing, every time.

## Verification method, for anyone re-running this

Real headless Chromium via Puppeteer (`npm install puppeteer`, not
present in this repo's own dependencies -- installed ad hoc for
verification), serving the repo root over real HTTP (`fetch()` and
Worker script loading both require a real origin, not `file://`), then
polling `window.term.buffer.active` (xterm.js's own line buffer,
exposed via `window.term = term` in `app.js` for exactly this kind of
external verification) until the expected checkpoint string appears or
a timeout elapses. This is a real, repeatable, scriptable browser test
-- not a one-off manual check -- but it isn't wired into this repo's
own test suite yet (no `puppeteer` dependency committed anywhere; it
was a verification tool for this session, not a new project
dependency). Worth revisiting if browser-specific regressions become a
real risk (e.g. once P4's F/D support adds more surface area).

## Still open

- The Puppeteer verification harness itself isn't checked in --
  worth adding as an optional `emulator/js/test-browser.js` (or
  similar) once it's clear this project wants a `puppeteer`
  devDependency at all, rather than assuming so unilaterally here.
- P4 (F/D instructions, needed for real userspace binaries beyond the
  kernel) -- unaffected by this phase, still not started.
- No keyboard-input-driven interactivity has been exercised yet (`kernel/`'s
  own test binaries never read from the UART -- `app.js`'s `term.onData`
  -> `worker.postMessage({type:'input',...})` path is wired up but
  untested against anything real).
