WASM_TCC := ../compiler/wasm32-tcc
I386_TCC ?= ../compiler/tcc
RV64_WASM := web/rv64.wasm

.PHONY: all native test test-boot test-shell clean

all: $(RV64_WASM)

$(WASM_TCC):
	$(MAKE) -C ../compiler TARGET=wasm32

$(RV64_WASM): rv64.c $(WASM_TCC)
	$(WASM_TCC) -nostdlib -o $@ $<

native: rv64-run

rv64-run: runner.c rv64.c
	$(I386_TCC) -O2 -o $@ runner.c

test: test-boot test-shell

test-boot: $(RV64_WASM)
	node test/boot.mjs ../kernel/kernel.elf

test-shell: $(RV64_WASM)
	node test/boot.mjs ../kernel/kernel-shell.elf 200000000 \
		'\r\recho hello ash\rls\recho writable-fs-works > wtest\rls\rexit 7\r' \
		'P10 checkpoint OK'

clean:
	rm -f $(RV64_WASM) rv64-run
