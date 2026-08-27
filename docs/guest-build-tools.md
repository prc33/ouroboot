# Building from inside Ouroboot

The practical first step is now implemented: the browser emulator offers a
small fetch device, exposed to the guest as:

    /fetch URL FILE

The browser performs HTTPS and copies the response into the guest's ramfs. This
keeps TCP, DNS, TLS, certificates, and HTTP out of the deliberately small
kernel. Cross-origin downloads still depend on the remote server's CORS policy;
same-origin URLs always work.

## Smallest route to running the build scripts

Pinned release archives plus BusyBox `tar` remain much smaller than bringing up
Git. The kernel now has the writable-file, directory, and metadata semantics
needed by archive extraction, and ramfs has capacity for full source trees.
The direct guest recipes below avoid requiring GNU make or patch in the guest.

## What unmodified Git would require

Compiling Git is not mainly a compiler task. A useful Git needs sockets, DNS,
polling, pipes, more complete process and filesystem syscalls, time and entropy,
plus zlib. HTTPS additionally needs libcurl and a TLS library with a certificate
store, or an external transport helper. Git's build also assumes a substantially
more complete `make`, shell, `sed`, `perl`-generated artifacts, and platform
feature probing.

If POSIX networking itself becomes a project goal, the clean route is a
virtio-net device in the emulator, a small network stack such as lwIP in the
kernel, and musl socket syscalls. That is much larger than the host-mediated
fetch boundary and is unnecessary for the stated self-hosting build closure.

## Current limitations

The fetch operation is browser-emulator-specific, synchronous from the guest's
point of view, and capped at 16 MiB per response. QEMU and the native emulator
runner do not yet implement the device. Downloads are not trusted until the
planned guest `sha256sum` verification is added; build scripts must use pinned
hashes rather than trusting mutable URLs.

## Proven source builds

The guest recipes live with the other build scripts in `demo/`:

- `build-musl-guest.sh` builds musl and runs a linked smoke test;
- `build-busybox-guest.sh` compiles the configured sources listed in
  `busybox-riscv64.sources`, links BusyBox directly, and runs the result.

After `demo/build-busybox-riscv64.sh` has prepared the pinned source tree, run
`demo/test-busybox-guest.sh`. It packs only the selected sources and required
headers into a 16-MiB-safe initrd, boots QEMU, and requires the BusyBox built
inside Ouroboot to execute successfully. The direct object link deliberately
avoids reproducing BusyBox's GNU make and archive-group machinery.
