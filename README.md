# BMDS — Bare Metal Do Stuff

A small, DOS-like operating system for the **ESP32-P4**, turning the [Olimex ESP32-P4-PC](https://www.olimex.com/Products/IoT/ESP32-P4/ESP32-P4-PC/open-source-hardware) into a self-contained personal computer: HDMI text console with graphics, USB keyboard and mouse, SD card storage, and separately-built programs loaded from disk. The long-term goal is a self-hosting system that can compile its own programs.

BMDS uses FreeRTOS (via ESP-IDF) as its kernel and provides everything above it: drivers, filesystem, consoles, shell, program loader, and a stable userspace ABI.

> **Status:** early bring-up. See [`devplan/roadmap.md`](devplan/roadmap.md).

## Principles

- **Don't break userspace.** Programs built for BMDS keep working on later versions. ([`docs/ABI.md`](docs/ABI.md))
- **The OS gets out of the way.** Minimal RAM, flash, and CPU overhead; idle means idle.
- **Qt-inspired C++20** internally, a **plain C ABI** for programs.

## Documentation

| Document | Contents |
|---|---|
| [`docs/VISION.md`](docs/VISION.md) | Goals, non-goals, high-level roadmap |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Layering, components, open design questions |
| [`docs/ABI.md`](docs/ABI.md) | Userspace ABI policy |
| [`docs/HARDWARE.md`](docs/HARDWARE.md) | Board and SoC reference |
| [`docs/BUILDING.md`](docs/BUILDING.md) | Toolchain setup, building, flashing, testing |
| [`CODING_STANDARDS.md`](CODING_STANDARDS.md) | Coding conventions |
| [`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md) | Third-party software and licences |
| [`devplan/`](devplan/) | Roadmap and subsystem design plans |

## License

MIT — see [`LICENSE`](LICENSE).
