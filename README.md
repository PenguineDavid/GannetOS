# GannetOS

GannetOS is an OS based on making a stable and customizable user experience and being very separate from things like linux and macOS but taking note of there mistakes.

The project is early-stage. The current target is **32-bit x86 in QEMU**. Native x86_64 kernel support is planned, but is not implemented yet.

## Build requirements

You need:

- GNU Make
- NASM
- QEMU
- A freestanding x86 cross-compiler and binutils toolchain
- A hosted C compiler for host tests

The cross tools are expected to use the `x86_64-elf-` prefix by default:

```text
x86_64-elf-gcc
x86_64-elf-g++
x86_64-elf-ld
x86_64-elf-objcopy
```

The name reflects the toolchain target. The kernel is still built as 32-bit x86 using `-m32` and `elf_i386`.

On Linux, install the packages available for your distribution and make sure these commands are on `PATH`. Cross-compiler package names vary by distribution, so building or installing an `x86_64-elf` toolchain may be necessary.

## Building

From the repository root:

```bash
make
make hosttests
```

Build the complete disk image and inject applications:

```bash
make flash
```

Boot the existing image in QEMU:

```bash
make run
```

Useful maintenance targets:

```bash
make clean
make fullclean
make check
```

The Makefile supports Linux/POSIX shells, Git Bash, and Windows `cmd.exe`. Tool names can be overridden without editing the file:

```bash
make CROSS_PREFIX=i686-elf- HOSTCC=gcc
make QEMU=qemu-system-i386 run
```

## Testing

`make hosttests` builds and runs the host-side tests. These cover selected logic such as syscall dispatch, user-pointer validation, task reaping, GDT encoding, and network wire formats.

## Current scope

The current implementation includes:

- 32-bit x86 boot and protected-mode kernel startup
- Paging and physical memory foundations
- Ring-3 applications and system calls
- Task scheduling and cleanup
- Filesystem, shell, pipes, and environment support
- Framebuffer, terminal, keyboard, mouse, and window-manager foundations
- RTL8139 networking with ARP, IPv4, ICMP, UDP, TCP, DNS, and HTTP work

Future work includes full per-process memory privacy, x86_64 support, broader hardware support, audio, TLS, and SSH.

## Project layout

```text
apps/       userland .pexe applications (echo, ls, ping, fetch, ...)
boot/       the bootloader (boot.asm)
filesys/    the on-disk filesystem and ATA driver
hosttest/   host-side (plain gcc, no QEMU) tests for host-testable logic
kernel/     the kernel itself - see kernel/README.md
shell/      the interactive shell and every ring0/ring3 privilege
            boundary crossing - see shell/README.md
tools/      build-time host tools (pexe packaging, disk image writing)
wm/         the tiled window manager
```

`kernel/README.md` and `shell/README.md` index those two directories'
headers by subsystem, with a one-line summary of each function - useful
for finding the right file before diving into its own, more detailed
comments.

## License

GannetOS is licensed under the [GNU General Public License v3.0](LICENSE).
See [CONTRIBUTING.md](CONTRIBUTING.md) for what that means for
contributions.

## Contributing

Before proposing a feature, read the [CONTRIBUTING.md](CONTRIBUTING.md).