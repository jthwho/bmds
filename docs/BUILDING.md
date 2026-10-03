# Building BMDS

> **Status:** the build skeleton doesn't exist yet. This document describes the intended workflow and will be updated as the project structure lands.

## Prerequisites

- **ESP-IDF v6.1** (currently developed against a v6.1-dev checkout at `~/esp/esp-idf`).
- A host C++20 compiler and CMake ≥ 3.22 for host-side unit tests.
- An Olimex ESP32-P4-PC connected over its USB-C port.

## Environment Setup

In an interactive shell:

```bash
esp-env        # sources ~/esp/esp-idf/export.sh plus local helpers
esp-select     # picks a board from /dev/esp-* and sets ESPPORT
```

In a non-interactive shell (scripts, CI, tools) where the `esp-env` shell function is not defined:

```bash
. ~/esp/esp-idf/export.sh
```

## Firmware

```bash
idf.py set-target esp32p4      # first time only
idf.py build
idf.py -p "$ESPPORT" flash monitor
```

Exit the monitor with `Ctrl-]`.

## Host Unit Tests

Hardware-independent code (`bmds_core` and other pure logic) is tested on the host with doctest:

```bash
cmake -S tests/host -B build-host
cmake --build build-host -j
ctest --test-dir build-host --output-on-failure
```

## Size Tracking

Resource usage is a first-class concern. Check the impact of changes with:

```bash
idf.py size
idf.py size-components
```
