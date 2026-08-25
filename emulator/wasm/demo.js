const output = document.querySelector('#output');

try {
    const response = await fetch('examples.wasm');
    if (!response.ok)
        throw new Error(`fetch failed: HTTP ${response.status}`);

    const { instance } = await WebAssembly.instantiateStreaming(response);
    const { add, factorial, polynomial, mix64, divide64 } = instance.exports;

    output.textContent = [
        `add(20, 22) = ${add(20, 22)}`,
        `factorial(6) = ${factorial(6)}`,
        `polynomial(3.5) = ${polynomial(3.5)}`,
        `mix64(123456789) = ${mix64(123456789n)}`,
        `divide64(-9000000000, 3) = ${divide64(-9000000000n, 3n)}`,
    ].join('\n');
} catch (error) {
    output.textContent = `Unable to run examples.wasm: ${error.message}`;
}
