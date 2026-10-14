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

## Recovery Firmware and Update Packages

The recovery firmware (`recovery/`, devplan/update.md §4) is a second IDF project over the same
components, run from `ota_1`, that applies updates. Build it in `build-recovery/`:

```bash
idf.py -B build-recovery -C recovery set-target esp32p4   # first time only
cd build-recovery && build
```

The main project's `idf.py flash` writes it to `ota_1` when `build-recovery/bmds_recovery.bin`
exists (looked for at configure time: reconfigure the main build after building recovery the
first time). `reboot recovery` starts it; its plain `reboot` goes back.

`build update` (in `build/`) makes `build/bmds.bmup`, a signed update package (devplan/update.md
§2) of the firmware, the ROM and (when built) the recovery firmware. It is signed with the dev key
(`keys/dev.key`) unless `-DBMDS_UPDATE_KEY=FILE` names another, and refused if the acceptance mode
(`CONFIG_BMDS_UPDATE_KEYS_*`, keys/README.md) wouldn't take that key. To install it on a board:

```bash
tools/bmds-update root@bmds0 build/bmds.bmup           # copy, check, apply, wait, show the result
tools/bmds-update --check root@bmds0 build/bmds.bmup   # copy and check only
```

`--force` installs a package that isn't newer than what is installed (`bmds/version.h`'s number).

## Host Unit Tests

Hardware-independent code (`bmds_core` and other pure logic) is tested on the host with doctest (vendored in `thirdparty/doctest/`, so no network access is needed). Use the `build` wrapper rather than a raw parallel build:

```bash
cmake -S tests/host -B build-host -G Ninja
cd build-host && build && ctest --output-on-failure
```

When Espressif's `riscv32-esp-elf-gcc` is found (on `PATH` after `export.sh`, or in `~/.espressif`), the host build also cross-compiles the SDK examples (`build-host/sdk-examples/`) and tests them with the loader's checks; it builds `build-host/bmds-elfcheck` either way.

## Programs (SDK)

Programs for BMDS are built with the SDK in `sdk/` (headers, startup code, linker script, CMake toolchain file); see [`sdk/README.md`](../sdk/README.md). `bmds-elfcheck program.elf` says whether the kernel will load a file, and why not.

## Configuration

`sdkconfig.defaults` is checked in; `sdkconfig` is generated and ignored. BMDS options are under **BMDS board**, **BMDS kernel**, **BMDS storage** (and others) in `idf.py menuconfig`:

- `BMDS_BOARD` selects the board definition.
- `BMDS_ASSERT` and `BMDS_DEBUG_LOG` compile in asserts and `bmdsDebug()`.
- `BMDS_LOG_LINE_MAX` sets the maximum log line length.
- `BMDS_TIMEZONE` is the kernel time zone at boot, a POSIX `TZ` string (under **BMDS kernel**).
- `BMDS_SD_MAX_KHZ` caps the SD bus clock; `BMDS_SD_BOUNCE_BYTES` sizes the SD bounce buffer
  (under **BMDS storage**).
- `BMDS_UPDATE_KEYS_DEV` / `BMDS_UPDATE_KEYS_RELEASE` choose which signing keys update packages
  are accepted with (keys/README.md); `BMDS_RECOVERY` is set by `recovery/sdkconfig.defaults`.

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
