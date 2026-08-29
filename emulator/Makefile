WASM_TCC := ../compiler/wasm32-tcc
I386_TCC ?= ../compiler/tcc
RV64_WASM := web/rv64.wasm

.PHONY: all native test test-boot test-shell test-fetch clean

all: $(RV64_WASM)

$(WASM_TCC):
	$(MAKE) -C ../compiler TARGET=wasm32

$(RV64_WASM): rv64.c $(WASM_TCC)
	$(WASM_TCC) -nostdlib -o $@ $<

native: rv64-run

rv64-run: runner.c rv64.c
	$(I386_TCC) -O2 -o $@ runner.c

test: test-boot test-shell

# kernel.elf is the direct product boot, so this smoke check reaches a real
# interactive ash session using the same self-hosting initrd as the browser demo.
test-boot: $(RV64_WASM)
	node test/boot.mjs ../kernel/kernel.elf 200000000 '' 'built-in shell (ash)' \
		../kernel/tcc-initrd.tar

test-shell: $(RV64_WASM)
	node test/boot.mjs ../kernel/kernel.elf 200000000 \
		'\r\recho hello ash\rls\recho writable-fs-works > wtest\rls\rexit 7\r' \
		'process: all processes exited' ../kernel/tcc-initrd.tar

# Exercises the browser-hosted fetch device without relying on the public
# network: Node's data: URL support follows the same asynchronous host path.
test-fetch: $(RV64_WASM)
	node test/boot.mjs ../kernel/kernel.elf 200000000 \
		'\r\r/fetch data:text/plain,hello-network fetched\rcat fetched\rexit 7\r' \
		'fetched 13 bytes to fetched\nhello-network# exit 7\nprocess: all processes exited' \
		../kernel/tcc-initrd.tar

clean:
	rm -f $(RV64_WASM) rv64-run
