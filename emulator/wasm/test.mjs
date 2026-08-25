import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const bytes = await readFile(new URL('examples.wasm', import.meta.url));
assert.equal(WebAssembly.validate(bytes), true);

const { instance } = await WebAssembly.instantiate(bytes);
assert.equal(instance.exports.add(20, 22), 42);
assert.equal(instance.exports.factorial(0), 1);
assert.equal(instance.exports.factorial(6), 720);
assert.equal(instance.exports.sum_squares(3, 4), 25);
assert.equal(instance.exports.polynomial(3.5), 20.25);
assert.equal(instance.exports.mix64(123456789n), 2195603616442482361n);
assert.equal(instance.exports.divide64(-9000000000n, 3n), -3000000000n);
assert.equal(instance.exports.identity64(0x123456789abcdefn), 0x123456789abcdefn);
assert.equal(instance.exports.add64(0x100000000n, 7n), 0x100000007n);
assert.equal(instance.exports.shift64(0x8000000000000000n, 63), 1n);
assert.equal(instance.exports.multiply64(0x100000001n, 3n), 0x300000003n);
assert.equal(instance.exports.extend_signed(-7), -7n);
assert.equal(instance.exports.extend_unsigned(0xffffffff), 0xffffffffn);
assert.equal(instance.exports.truncate64(0x10000002an), 42);
assert.equal(instance.exports.i64_to_double(-9007199254740991n), -9007199254740991);
assert.equal(instance.exports.double_to_i64(-12345.75), -12345n);
console.log('wasm32 examples: PASS');
