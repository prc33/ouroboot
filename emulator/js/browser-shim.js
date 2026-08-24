'use strict';
/* Zero-build-step CommonJS-in-the-browser loader. worker.js calls
 * loadCjsModule(name) for each core file in dependency order -- each
 * one runs in its OWN isolated function scope (exactly Node's own
 * per-file module wrapper technique: `(function(module, exports,
 * require){ ... })`), not naively via importScripts().
 *
 * importScripts() was the first attempt and looked fine until real
 * multi-file loading was tested: it executes every file directly in
 * the Worker's single shared global scope with no per-file isolation,
 * so a *local-looking* top-level declaration in one file can collide
 * with another file's -- confirmed the hard way: cpu.js's own
 * `const { CSR, ... } = require('./csr')` (a perfectly ordinary,
 * module-private destructuring assignment under Node, where each file
 * already has its own closure) became a *second* global `const CSR`
 * binding in the shared Worker scope, colliding with csr.js's own
 * top-level `const CSR = {...}` and throwing
 * "Identifier 'CSR' has already been declared" -- found via real
 * headless-Chromium testing (Puppeteer), not reproducible under plain
 * Node (where require() naturally isolates each file). The same class
 * of bug would recur for any other file pair sharing an unremarkable
 * local name (`mask`, `PAGE_SIZE`, etc) -- not a one-off, hence fixing
 * the loading mechanism itself rather than renaming variables.
 *
 * Each core file is otherwise completely untouched -- same file
 * Node's boot.js and kernel/Makefile's test-js target already
 * exercise.
 */

self.__cjsModules = {};

/* Synchronous XHR is deprecated on the main thread but still
 * supported (and not deprecated) inside a Worker -- exactly the one
 * place it's still the right tool, since Worker top-level code has no
 * top-level await and these files must load in a strict dependency
 * order before anything can use them. */
function fetchTextSync(url) {
	const xhr = new XMLHttpRequest();
	xhr.open('GET', url, false);
	xhr.send(null);
	if (xhr.status !== 200)
		throw new Error(`loading ${url}: HTTP ${xhr.status}`);
	return xhr.responseText;
}

/* Loads <name>.js, in its own function scope, and returns its
 * module.exports -- callable from worker.js like
 * `const { Memory } = loadCjsModule('memory');`. Idempotent (a
 * second call for an already-loaded name returns the cached exports
 * instead of re-fetching/re-executing) -- harmless if a future file
 * ends up require()'d from two different places. */
self.loadCjsModule = function (name) {
	if (self.__cjsModules[name])
		return self.__cjsModules[name];

	const source = fetchTextSync(name + '.js');
	const module = { exports: {} };
	const localRequire = function (path) {
		const dep = path.replace(/^\.\//, '').replace(/\.js$/, '');
		return self.loadCjsModule(dep);
	};
	/* `new Function` instead of eval: same isolation property
	 * (its own scope, no access to this function's locals beyond
	 * what's explicitly passed as parameters), clearer intent. */
	const fn = new Function('module', 'exports', 'require', source);
	fn(module, module.exports, localRequire);

	self.__cjsModules[name] = module.exports;
	return module.exports;
};
