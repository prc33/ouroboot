// Loads corpus.wasm, calls every exported `test_*` function (all of them
// take no arguments and return a plain i32 -- see corpus.c's own header
// comment for why), and prints "name=value" lines in the same format
// run.sh's native driver uses, sorted so line order can never cause a
// spurious diff. Exports are discovered from the module itself, not a
// hand-maintained list -- see gen_driver.py's own comment for why that
// matters.
import { readFile } from 'node:fs/promises';

const bytes = await readFile(new URL('corpus.wasm', import.meta.url));
const { instance } = await WebAssembly.instantiate(bytes);

const names = Object.keys(instance.exports)
    .filter((n) => n.startsWith('test_'))
    .sort();
if (names.length === 0) {
    console.error('run_wasm.mjs: no test_* exports found in corpus.wasm');
    process.exit(1);
}
for (const name of names) {
    const v = instance.exports[name]();
    console.log(`${name}=${v}`);
}
