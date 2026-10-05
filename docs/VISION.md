# BMDS Vision

**BMDS — "Bare Metal Do Stuff"** — is a small operating system for the ESP32-P4 that turns a cheap development board into a self-contained personal computer. You plug in an HDMI monitor, USB keyboard, and mouse, insert an SD card, and get a computer you can program **on the machine itself**.

The model is **DOS, not Linux**: one user, a text console with graphics available, programs loaded from disk that get (nearly) the whole machine, and an OS that provides services and then gets out of the way.

## Goals

- **A PC-like experience.** Boot to a prompt on an HDMI text console. Keyboard and mouse over USB. Files on an SD card. Run programs by typing their name.
- **Separately built executables.** Programs are built independently of the OS (initially cross-compiled on a host, eventually on the device), stored on the SD card, loaded, and run.
- **Self-hosting.** The long-term goal is to edit, compile, and run programs entirely on the device — a native C compiler, assembler/linker, and editor running on BMDS.
- **Text first, graphics too.** A fast text console is the primary interface. Programs can also switch to graphics modes and draw directly, DOS-style.
- **Efficient to the bone.** The OS uses as little RAM, flash, and CPU as possible. Idle means idle. Unused features cost nothing.
- **Don't break userspace.** Once a program runs on BMDS, it keeps running on every later version of BMDS. (See `docs/ABI.md`.)
- **Fast iteration.** Developing BMDS itself, and programs for it, should be quick: a serial shell from day one, the ability to push a program over serial and run it without reflashing, and host-side unit tests for anything that doesn't need hardware.
- **Portable across boards.** All board-specific facts live in one board definition. Supporting a new ESP32-P4 board (or a later chip) should mean writing a new board definition, not rewriting drivers.

## Non-Goals (for now)

- **Memory protection between programs.** There's no per-program address space: the P4's MMU only maps flash and PSRAM, with one table for both cores and no write protection. Like DOS, a user program can in principle crash the machine. (The P4's RISC-V PMP *may* allow some kernel self-protection later — see `docs/ARCHITECTURE.md` open questions.)
- **Multi-user, permissions, or security boundaries.**
- **POSIX compliance.** We may provide a POSIX-like C library subset for porting convenience, but the native API is our own.
- **Wi-Fi / Bluetooth.** The ESP32-P4 has no radio, and the Olimex ESP32-P4-PC has no companion wireless chip. This is deferred until a board with wireless support is targeted. The architecture should not preclude it.
- **Running unmodified Linux/Windows binaries.**

## Feature Roadmap (High Level)

The ordering is deliberate — each stage makes the next one faster to build. See `devplan/roadmap.md` for detail.

1. **Foundation** — project skeleton, board definition, core library, host tests, boot + logging.
2. **Serial shell** — interactive shell over the USB-Serial/JTAG port, used to drive everything after this.
3. **Storage** — SD card driver and filesystem; file commands in the shell.
4. **Programs** — executable loader, syscall ABI v1, userspace SDK, push-over-serial-and-run.
5. **Display** — HDMI output, framebuffer, text console.
6. **Input** — USB host, hub, HID keyboard and mouse; the shell running on keyboard + HDMI.
7. **Graphics** — graphics modes and a basic drawing API for programs.
8. **Sound and network** — audio output, Ethernet.
9. **Self-hosting** — editor, C compiler, assembler/linker running on BMDS.

## Self-Hosting Notes

The ESP32-P4's HP cores are 32-bit RISC-V (RV32IMAFC plus Espressif extensions). Candidate compiler paths, to be evaluated when we get there:

- **TinyCC** — small, fast, self-contained compiler/assembler/linker; has a RISC-V *64* backend that would need porting to RV32.
- Other small C compilers (chibicc-, cproc/QBE-, or lcc-style) would also need an RV32 backend.
- Whatever we choose shapes the SDK: the userspace ABI and SDK headers are deliberately plain C99-compatible so a small compiler can consume them.
