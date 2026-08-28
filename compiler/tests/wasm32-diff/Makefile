TCC := ../../wasm32-tcc

.PHONY: all test clean

all: corpus.wasm

$(TCC):
	$(MAKE) -C ../.. TARGET=wasm32

corpus.wasm: corpus.c $(TCC)
	$(TCC) -nostdlib -o $@ $<

test:
	bash run.sh

clean:
	rm -f corpus.wasm driver.c native_ref native.out wasm.out
