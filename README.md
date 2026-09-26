> **LinuxOnTab fork.** Branch `wasm-linuxontab` of this repository is the
> GPL-2.0 corresponding source for the kernel binary shipped by
> [LinuxOnTab 2.0](https://github.com/kilian-ai/linuxontab) at
> [next.linuxontab.com](https://next.linuxontab.com)
> (`shell/linux-dist/vmlinux.wasm`). Every shipped binary embeds the exact
> commit it was built from in a `.linuxontab.source` custom section, and
> `kernels/vmlinux.source` in the LinuxOnTab repo records the same pointer.
> Build and ship instructions: [kernels/README.md](https://github.com/kilian-ai/linuxontab/blob/feature/linux-kernel-integration/kernels/README.md).
> Additions over upstream live under `arch/wasm/` and `tools/wasm/` (syscall
> restart, futex, kernel clocks for the JS shims, signal-frame save/restore,
> kdiag boot telemetry, larger devicetree buffer, ARCH_FORCE_MAX_ORDER=14,
> macOS build fixes). The port itself is Thomas Stokes' work — everything
> below is the upstream README.

# Linux WebAssembly port

[Try it in your browser.](https://linux.tombl.dev)

- [x] All the build infrastructure
- [x] Reimplementation of the ELF/linker features the kernel relies on
- [x] Kernel threads
  - Mapped to JavaScript workers
  - Complete multicore support
- [x] Implement virtio in the host library
  - [x] Implement a block device
- [x] `binfmt_wasm`
- [x] Jumping into userspace
- [x] Expose syscall dispatch
- [x] [Port musl](https://github.com/tombl/musl)
- [x] [Port busybox](https://github.com/tombl/busybox)

## Eventually:

- signals
- virtio-net to connect multiple machines
  - and service worker loopback
- enhanced virtio-console to support terminal resizing and multiple terminals
- virtio-fs backed by the [File System API](https://developer.mozilla.org/en-US/docs/Web/API/File_System_API)
- vsock to implement custom javascript integrations
  - unrestricted vscode in the browser?
- [port lots of software](https://github.com/tombl/distro)
  - wrap/patch compilers to support `wasm32-linux`
    - both for cross-compilation and self-hosting
  - tailscale for full networking?
  - an x86_64 emulator like qemu/blink/box64?
- `/dev/fetch` as a character device exposing a HTTP proxy interface
  - parses requests and invokes `fetch()`
- canvas2d framebuffer driver?
- virtio-gl backed by WebGL?
- [wayland?](https://github.com/udevbe/greenfield)
- MMU support?
  - hopefully [WebAssembly/memory-control](https://github.com/WebAssembly/memory-control) is enough
  - otherwise instrument all memory access and implement a simple software MMU
- support dynamic linking
  - might need to wait for the [ABI](https://github.com/WebAssembly/tool-conventions/blob/main/DynamicLinking.md) to stabilize

---

> [If we add another architecture in the future, it may instead
> be something like the LLVM bitcode or WebAssembly, who knows?](https://lore.kernel.org/all/CAK8P3a2-wyXxctVtJxniUoeShASMhF-6Z1vyvfBnr6wKJuioAQ@mail.gmail.com/)
