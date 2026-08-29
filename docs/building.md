# Building and testing

You need a POSIX shell, a host C compiler, Make, Git, Bash, Python 3, and a
modern browser. The first build fetches pinned musl and BusyBox source trees.
Node.js is needed for browser-emulator tests; QEMU is needed only for QEMU
tests.

## Browser system

From the repository root:

```sh
make -C compiler TARGET=riscv64
./demo/build-musl-riscv64.sh
./demo/build-busybox-riscv64.sh
make -C kernel ARCH=riscv64 kernel.elf tcc-initrd
make -C compiler TARGET=wasm32
make -C emulator
python3 -m http.server 8000
```

Open `http://localhost:8000/emulator/web/`. At the BusyBox prompt:

```sh
ls
cat hello.c
ash /selfhost.sh
```

## Native emulator

Build the same guest, then either run `make -C emulator native` with the i386
TCC target and an i386 libc available, or use a host compiler for development:

```sh
cc -O2 -o emulator/rv64-run emulator/runner.c
./emulator/rv64-run kernel/kernel.elf kernel/tcc-initrd.tar
```

## Checks

```sh
make -C compiler TARGET=riscv64 selfcheck
make -C compiler TARGET=i386 selfcheck
make -C kernel ARCH=riscv64 test-selfhost
make -C emulator test
```

`test-selfhost` needs `qemu-system-riscv64`. The compiler self-checks use the
matching `qemu-*-static` executable. `make -C kernel ARCH=riscv64 test-wasm`
boots the checkpoint test image through the C/Wasm emulator.
