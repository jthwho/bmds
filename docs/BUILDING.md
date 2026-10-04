# Building BMDS

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

Hardware-independent code (`bmds_core` and other pure logic) is tested on the host with doctest (vendored in `thirdparty/doctest/`, so no network access is needed). Use the `build` wrapper rather than a raw parallel build:

```bash
cmake -S tests/host -B build-host -G Ninja
cd build-host && build && ctest --output-on-failure
```

## Configuration

`sdkconfig.defaults` is checked in; `sdkconfig` is generated and ignored. BMDS options are under **BMDS board**, **BMDS kernel**, **BMDS storage** (and others) in `idf.py menuconfig`:

- `BMDS_BOARD` selects the board definition.
- `BMDS_ASSERT` and `BMDS_DEBUG_LOG` compile in asserts and `bmdsDebug()`.
- `BMDS_LOG_LINE_MAX` sets the maximum log line length.
- `BMDS_TIMEZONE` is the kernel time zone at boot, a POSIX `TZ` string (under **BMDS kernel**).
- `BMDS_SD_MAX_KHZ` caps the SD bus clock; `BMDS_SD_BOUNCE_BYTES` sizes the SD bounce buffer
  (under **BMDS storage**).

## Release Builds

Development builds keep diagnostics that a release build drops. There is no release
configuration yet; when there is, it sets:

- `CONFIG_COMPILER_OPTIMIZATION_CHECKS_SILENT=y`: drops the message strings of ESP-IDF's
  `ESP_RETURN_ON_*` checks (16 KB of a 320 KB image, measured in Phase 2). BMDS still logs its own
  error with the `esp_err_t`.

## Size Tracking

Resource usage is a first-class concern. Check the impact of changes with:

```bash
idf.py size
idf.py size-components
```

The baseline is recorded in `devplan/foundation.md`.
