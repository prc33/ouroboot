WASM_TCC := ../compiler/wasm32-tcc

.PHONY: all native test test-boot test-shell test-fetch clean

all: web/rv64.wasm

$(WASM_TCC):
	$(MAKE) -C ../compiler TARGET=wasm32

web/rv64.wasm: rv64.c $(WASM_TCC)
	$(WASM_TCC) -nostdlib -o $@ $<

native: rv64-run

rv64-run: runner.c rv64.c
	../compiler/tcc -O2 -o $@ runner.c

test: test/paging test-boot test-shell
	./test/paging

test/paging: test/paging.c rv64.c
	$(CC) -O2 -o test/paging test/paging.c

# kernel.elf is the direct product boot, so this smoke check reaches a real
# interactive ash session using the same self-hosting initrd as the browser demo.
test-boot: web/rv64.wasm
	node test/boot.mjs ../kernel/kernel.elf 200000000 '' 'built-in shell (ash)' \
		../kernel/tcc-initrd.tar

test-shell: web/rv64.wasm
	node test/boot.mjs ../kernel/kernel.elf 400000000 \
		'\r\recho hellp\bo ash\rfind . -name "*.c" | wc -l\rtouch touched\rsleep 1\rtest -f touched && echo sleep-touch-ok\rls\recho writable-fs-works > wtest\rls\rexit 7\r' \
		'hello ash\n28\n# touch touched\nelf: loaded, entry=0x100b0\n# sleep 1\nelf: loaded, entry=0x100b0\n# test -f touched && echo sleep-touch-ok\nsleep-touch-ok\n# ls\nprocess: all processes exited' ../kernel/tcc-initrd.tar

# Exercises the browser-hosted fetch device without relying on the public
# network: Node's data: URL support follows the same asynchronous host path.
test-fetch: web/rv64.wasm
	node test/boot.mjs ../kernel/kernel.elf 200000000 \
		'\r\r/fetch data:text/plain,hello-network fetched\rcat fetched\rexit 7\r' \
		'fetched 13 bytes to fetched\nhello-network# exit 7\nprocess: all processes exited' \
		../kernel/tcc-initrd.tar

clean:
	rm -f web/rv64.wasm rv64-run test/paging
