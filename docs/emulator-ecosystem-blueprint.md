<!--
Kept verbatim for reference -- this is the blueprint an external agent
produced when given a summary of this project's tcc fork + kernel work
and asked about adding a browser RISC-V emulator + eventual Linux
support. docs/emulator-plan.md adapts this into the project's own
conventions (locked decisions, phased exit criteria, derive-before-
build) and reconciles it with docs/self-hosting-system-plan.md -- read
that first. This file is the original raw input, not itself a plan
this project follows step-for-step.
-->

# Project Blueprint: The Self-Hosting Web RISC-V Ecosystem

```
+-------------------------------------------------------------+
|                     HTML5 Browser Window                    |
|  +-------------------------------------------------------+  |
|  |             Minimal JS RISC-V Emulator                |  |
|  |  +-------------------------------------------------+  |  |
|  |  |             Minimal Kernel / Linux              |  |  |
|  |  |  +-------------------------------------------+  |  |  |
|  |  |  |   BusyBox Shell <----> Tiny C Compiler    |  |  |  |
|  |  |  +-------------------------------------------+  |  |  |
|  |  +-------------------------------------------------+  |  |
|  +-------------------------------------------------------+  |
+-------------------------------------------------------------+
```

## Phase 1: Toolchain Validation (Host Side)
Before moving into the browser, establish your core userland and kernel compiler constraints on your native host machine.

*   **[ ] Musl & BusyBox Bootstrap**:
    *   Compile `musl` libc with your `tcc` fork. Fix any `tcc` limitations regarding advanced GNU C extensions or assembly syntax often found in libc string/math headers.
    *   Compile `busybox` statically using your `tcc` + `musl` toolchain. Strip all non-essential applets to minimize binary footprint.
*   **[ ] Kernel Compilation**:
    *   **Custom Minimal Kernel**: Write/refine your kernel using simple C structures. Ensure it supports basic RISC-V supervisor mode tasks, page tables, a basic timer, and a UART driver.
    *   **Linux Target**: Begin auditing standard Linux kernel RISC-V code. Linux heavily utilizes GCC-specific inline assembly, complex macros, and VLA (Variable Length Arrays) which `tcc` traditionally struggles with. Maintain a list of necessary patches or configurations (e.g., disabling heavy subsystems).

## Phase 2: Building the Minimal Browser Emulator
Instead of pulling in a massive emulator, extract and adapt the lightweight core mechanics found in `jor1k`.

*   **[ ] Simplify the CPU Loop**:
    *   Extract the RISC-V execution loop from `jor1k` or build a clean one from scratch using standard JavaScript or WebAssembly.
    *   Focus strictly on **RV32IMA** or **RV64IMA**. You do not need Float/Double (`F`/`D`) support initially; compile `musl` and your kernels with soft-float flags (`-msoft-float`) to save massive emulator complexity.
    *   Implement Privileged Architecture: Machine (`M`), Supervisor (`S`), and User (`U`) modes, alongside basic MMU page table walking.
*   **[ ] Implement Minimal I/O**:
    *   **UART 16550**: Provide a basic serial input/output mapped to a web terminal interface (like `xterm.js`).
    *   **VirtIO-Block (Optional but recommended)**: A basic ramdisk (initramfs) bundled directly with the kernel image is easiest for early boots. Later, implement a simple HTTP-backed VirtIO block device to stream file modifications.

## Phase 3: The "Boot & Shell" Milestone
Bring the pieces together inside the browser environment.

*   **[ ] Image Packaging**:
    *   Bundle your compiled kernel and your static BusyBox `initramfs` into a uniform boot image.
*   **[ ] First Browser Boot**:
    *   Load your JS emulator in an HTML page, point it at the image, and boot into your custom kernel.
    *   Succeed when you reach a functional, responsive BusyBox shell (`/bin/sh`) inside the browser terminal window.

## Phase 4: Achieving Self-Compilation (The Holy Grail)
The ultimate test of the ecosystem: compiling itself entirely inside the sandbox.

*   **[ ] Porting the Compiler**:
    *   Take your `tcc` source code, compile it using your host-side `tcc` toolchain, and place the resulting `tcc` binary inside your BusyBox `initramfs` image.
*   **[ ] In-Emulator Self-Hosting**:
    *   Boot up the browser emulator.
    *   From inside the BusyBox shell, pull or store the `tcc` source code files.
    *   Run `/bin/tcc -o tcc_native tcc.c` within the browser environment.
    *   Verify that `tcc_native` runs cleanly and can compile your custom kernel directly inside the running webpage.

---

### Technical Pitfalls to Watch For

1.  **Alignment & Soft-Float**: Native `tcc` targets standard chipsets. Ensure your `musl` build flag combination explicitly strips out hardware floating-point instructions (`-mabi=lp64 --with-arch=rv64ima`) so your emulator doesn't crash on unhandled float opcodes.
2.  **Linux vs. Custom Kernel Macro Bloat**: Standard Linux kernels make extensive use of GCC-specific extensions (`__builtin_...`). Compiling modern Linux with `tcc` requires a massive amount of header stubbing. Sticking with your custom minimal kernel first will give you rapid, encouraging results.
