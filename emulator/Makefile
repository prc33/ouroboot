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

# kernel.elf is the product boot (no historical checkpoint chain --
# see docs/kernel-complexity-review.md section 1), so the smoke check
# here is that it reaches a real interactive ash session, not the old
# "P5 checkpoint 2 OK" milestone from before that boot existed.
test-boot: $(RV64_WASM)
	node test/boot.mjs ../kernel/kernel.elf 200000000 '' 'built-in shell (ash)' \
		../kernel/initrd.tar

test-shell: $(RV64_WASM)
	node test/boot.mjs ../kernel/kernel.elf 200000000 \
		'\r\recho hello ash\rls\recho writable-fs-works > wtest\rls\rexit 7\r' \
		'process: all processes exited' ../kernel/initrd.tar

clean:
	rm -f $(RV64_WASM) rv64-run
