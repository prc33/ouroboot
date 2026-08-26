# Building from inside Ouroboot

The practical first step is now implemented: the browser emulator offers a
small fetch device, exposed to the guest as:

    /fetch URL FILE

The browser performs HTTPS and copies the response into the guest's ramfs. This
keeps TCP, DNS, TLS, certificates, and HTTP out of the deliberately small
kernel. Cross-origin downloads still depend on the remote server's CORS policy;
same-origin URLs always work.

## Smallest route to running the build scripts

The existing `demo` scripts should be changed from `git clone` plus `git apply`
to downloading pinned release archives, checking their SHA-256 hashes, and
unpacking them with BusyBox `tar`. This preserves reproducibility and requires
far less guest functionality than Git. The remaining tools are:

- BusyBox applets for `make`, `patch`, `sha256sum`, `awk`, `yes`, and `nproc`;
- TCC's already-present compiler, linker, `ar`, and `ranlib` modes;
- writable directories, which ramfs currently infers but cannot create as
  persistent empty objects (adequate for populated build trees, but `mkdir`
  semantics should be completed before running unmodified build systems);
- enough ramfs metadata and non-contiguous file storage for thousands of source
  and object files. The current 512-file limit and contiguous-growth allocator
  are the next important constraints.

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
