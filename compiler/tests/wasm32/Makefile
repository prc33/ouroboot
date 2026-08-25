TCC := ../../wasm32-tcc

.PHONY: all test serve clean

all: examples.wasm

$(TCC):
	$(MAKE) -C ../.. TARGET=wasm32

examples.wasm: examples.c $(TCC)
	$(TCC) -nostdlib -o $@ $<

test: examples.wasm
	node test.mjs

serve: examples.wasm
	python3 -m http.server 8000

clean:
	rm -f examples.wasm
