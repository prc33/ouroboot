TCC := ../../compiler/wasm32-tcc
RV64_WASM := ../js/rv64.wasm

.PHONY: all test test-boot test-shell serve clean

all: examples.wasm $(RV64_WASM)

$(TCC):
	$(MAKE) -C ../../compiler TARGET=wasm32

examples.wasm: examples.c $(TCC)
	$(TCC) -nostdlib -o $@ $<

$(RV64_WASM): rv64.c $(TCC)
	$(TCC) -nostdlib -o $@ $<

test: examples.wasm
	node test.mjs

test-boot: $(RV64_WASM)
	node boot.mjs ../../kernel/kernel.elf

test-shell: $(RV64_WASM)
	node boot.mjs ../../kernel/kernel-shell.elf 200000000 \
		'\r\recho hello ash\rls\recho writable-fs-works > wtest\rls\rexit 7\r' \
		'P10 checkpoint OK'

serve: examples.wasm
	python3 -m http.server 8000

clean:
	rm -f examples.wasm $(RV64_WASM)
