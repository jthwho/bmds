# Phase 0: Foundation

Status: **implemented and verified on hardware** (Olimex ESP32-P4-PC, chip revision v1.3).

## 1. Goal

Provide the skeleton everything else is built on:
- an ESP-IDF project for the ESP32-P4 with the BMDS build policy;
- the hardware-independent core types (`bmds_core`), with host unit tests;
- the board definition, holding all hardware truth (`bmds_board`);
- the first kernel pieces (`bmds_kernel`): logging, MemSpace allocation, and the boot sequence with its timing/memory report.

## 2. Interface

### `bmds_core` (no IDF headers; host-tested)

| Header | Provides |
|---|---|
| `bmds/config.h` | `BMDS_NAMESPACE_BEGIN/END`, `BMDS_STRINGIFY`, `BMDS_SOURCE_FILE`, `BMDS_PRINTF_FORMAT`, `BMDS_PLATFORM_ESP/HOST`, `BMDS_ENABLE_ASSERT`, `BMDS_ENABLE_DEBUG_LOG`, `bmds::WaitForever` |
| `bmds/assert.h` | `BMDS_ASSERT(cond)`; the platform hook `Detail::assertFailed()` |
| `bmds/error.h` | `Error` (1 byte, `[[nodiscard]]`), `fromEspErr()`, `syserr()`, `name()`, `desc()` |
| `bmds/result.h` | `Result<T>` (`[[nodiscard]]`, structured bindings), `makeResult()`, `makeError<T>()` |
| `bmds/stringview.h` | `StringView` (constexpr, Qt-style: `left/right/mid`, `startsWith`, `indexOf`, `trimmed`, ...) |
| `bmds/span.h` | `Span<T>`, `ByteSpan`, `WritableByteSpan` |
| `bmds/memspace.h` | `enum class MemSpace` (`Internal`, `InternalDma`, `Psram`, `PsramDma`, `Fast`), `memSpaceName()` |
| `bmds/version.h` | `BMDS_VERSION_*`, `bmds::VersionString` |
| `bmds/detail/esperr.h` | Numeric `esp_err_t` values used by `fromEspErr()` |

### `bmds_board`

- `bmds/board.h`:
  - `Board` and its parts: `GpioLine`, `BoardI2cBus`, `BoardSdCard`, `BoardDisplay`, `BoardUsbHost`, `BoardEthernet`, `BoardAudio`, `BoardMemSpace`.
  - `bmds::board()`, a constexpr accessor for the selected board.
- `boards/<board>/bmds/boarddef.h`: the board's `constexpr Board`, selected by Kconfig `BMDS_BOARD`.

### `bmds_kernel`

- `bmds/log.h`:
  - Macros: `bmdsDebug/Info/Warn/Err`, `BMDS_DEBUG(Module)`.
  - `Log::setLevel()`.
  - `Log::debugModules()` and `Log::findDebugModule()`; the Phase 1 `log` command will use these.
- `bmds/memory.h`: `Memory::alloc/allocAligned/free/stats(MemSpace)`. (Phase 1b moved the declaration to `bmds_core` so the containers can allocate; the definition stays here.)
- `bmds/boot.h`: `Boot::run()`, called from `app_main()`.

## 3. Design

- **`Result<T>` has no implicit constructor from `Error`.** `Error::Code` is an unscoped enum (so call sites can write `Error::NoMem`), and an unscoped enum converts implicitly to `int`. With such a constructor, `Result<int> r = Error::NoMem;` would quietly construct the *value* 2. Following promeki, construction goes through `makeResult()` and `makeError<T>()`.
- **`Error` and `Result` are `[[nodiscard]]`.** With `-Werror`, an ignored error doesn't compile. Discard explicitly with `(void)`.
- **`fromEspErr()` without IDF headers.** The `esp_err_t` values are duplicated in `bmds/detail/esperr.h`, and `bmds_kernel/src/esperr_check.cpp` static_asserts each one against `esp_err.h`. This keeps the function host-testable, and the target build catches any drift.
- **`syserr()` uses a lookup table, not a `switch`.** errno macros alias differently per C library (glibc has `ENOTSUP == EOPNOTSUPP`, newlib doesn't), which would cause duplicate case labels on one platform or the other.
- **Hard-won: `MINIMAL_BUILD` silently drops Kconfig options of components that aren't required.** `CONFIG_SPIRAM=y` in `sdkconfig.defaults` did nothing until `bmds_kernel` required `esp_psram`, because the option didn't exist. The first boot found no PSRAM; the boot `memory` stage caught it. After adding a setting to `sdkconfig.defaults`, check that it appears in the generated `sdkconfig`.
- **Debug-module registry with no global constructors:**
  - `BMDS_DEBUG(Module)` defines a per-file `constinit bool` flag in BSS. It also places a `constexpr DebugModule {name, &flag}` descriptor in section `.bmds_debug`.
  - The `bmds_kernel/linker.lf` fragment collects that section into `flash_rodata`, bounded by `_bmds_debug_start/_end`.
  - The cost per module is 8 bytes of flash plus 1 byte of RAM, and `bmdsDebug()` tests the flag directly.
  - **Hard-won:** the first version put mutable descriptors in `dram0_data`. On the ESP32-P4, IDF's linker template emits the `dram0_data` placement **twice** (low and high SRAM), so `SURROUND` defines the bounds symbols twice and the second, empty pair wins. `flash_rodata` is emitted only once.
- **Log line format:** `[  sec.mmm] L msg`. The timestamp is `esp_log_timestamp()`, milliseconds since reset, the same clock as IDF's own log lines. Debug lines include `Module: `. Warnings and errors append `(file:line)`.
  - Each line is formatted into a stack buffer of `CONFIG_BMDS_LOG_LINE_MAX` bytes and written with one `fwrite` to stdout; stdout's lock keeps lines from interleaving.
  - There is no allocation. Logging is not ISR-safe.
  - Phase 1 replaces the stdout write with a log ring buffer feeding the consoles.
- **The board definition is constexpr data.**
  - `Board` is an aggregate built with designated initializers, as an `inline constexpr` value in rodata.
  - Signals with a polarity are `GpioLine {gpio, activeLow}`. Optional hardware is marked with `NoGpio`, `NoLdo`, or a 0 count.
  - The MemSpace→heap-caps mapping is part of the board, because whether PSRAM exists is a board fact.
  - `board.h` includes the selected `boarddef.h` at its end, through a Kconfig-selected include directory.
- **MemSpace mapping:**

  | Space | Heap caps |
  |---|---|
  | Internal | `INTERNAL\|8BIT` |
  | InternalDma | `INTERNAL\|DMA\|8BIT` |
  | Psram | `SPIRAM\|8BIT` |
  | PsramDma | `SPIRAM\|DMA\|CACHE_ALIGNED` |
  | Fast | `TCM` |

  - `esp_mm` aligns DMA allocations automatically.
  - `MALLOC_CAP_CACHE_ALIGNED` is an allocation hint, not a region capability, so `Memory::stats()` masks it off.
  - **Hard-won:** no heap region is marked `SPIRAM|DMA`. On allocation `esp_mm` drops `DMA` from SPIRAM requests, because P4 PSRAM is DMA-capable. Region queries skip that adjustment, so `Memory::stats()` mirrors it; otherwise `PsramDma` reports 0.
  - The spaces overlap physically, so their stats are not additive.
  - `Internal` can fall back to LP SRAM (RTCRAM, slower), which the heap uses only at the lowest priority.
- **Boot sequence:**
  - A rodata table of `{name, Error (*init)()}` stages, run in order and each timed.
  - It then prints a report: versions, chip revision, stage times, time since boot, per-MemSpace memory, the debug module count, and the main task stack high-water mark.
  - `app_main()` returns afterwards, so IDF deletes the main task and reclaims its stack.
  - Phase 0 has one real stage, `memory`, which verifies that every MemSpace has backing memory.

## 4. IDF Usage

| IDF piece | Use | Why |
|---|---|---|
| `MINIMAL_BUILD` | Build only what `main` requires | Faster builds; IDF builds about 40 components instead of all of them |
| `esp_psram` | PSRAM init and heap pool | Required by `bmds_kernel` (see the hard-won note in Design) |
| `heap` (`heap_caps_*`) | Behind `Memory` | Correct region and capability handling, plus DMA alignment through `esp_mm`; nothing to gain by replacing it |
| `esp_timer` | Log timestamps, boot timing | |
| `esp_app_format`, `esp_hw_support` | Banner (git-describe version, IDF version, chip revision) | |
| stdio (`esp_stdio`, USB-Serial/JTAG console) | Log output | Temporary, until the Phase 1 console layer |
| Linker fragments | Debug-module registry | |

### Build Policy

- `cmake/bmds.cmake: bmds_target_options()` adds `-std=gnu++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor` to BMDS targets.
- IDF v6.1 defaults to `-std=gnu++26`. Our flag comes later on the command line and wins; this was verified in `compile_commands.json`.
- `bmds_core/CMakeLists.txt` works both as an IDF component and as a plain CMake library, selected by `if(COMMAND idf_component_register)`. This works during IDF's early expansion, where `ESP_PLATFORM` is not yet set.

### sdkconfig.defaults Choices

- **Flash and partitions:** 16 MB flash and a custom partition table (`nvs` plus a 4 MB `factory` app; the rest is unallocated for now). Since 2026-10-09: `rom.md` §3a (the main firmware in `ota_0`, recovery in `ota_1`, `otadata`, `/boot`, the ROM).
- **PSRAM at 80 MHz.** The IDF default is 20 MHz; 200 MHz needs `IDF_EXPERIMENTAL_FEATURES`.
- **PSRAM boot memory test disabled** (`SPIRAM_MEMTEST=n`). It cost about 1.3 s of every boot.
- **Console:** USB-Serial/JTAG.
- **Optimization:** `-Os`.
- **FreeRTOS tick:** 1 kHz, so `timeoutMs` is honoured at 1 ms granularity.
- **Chip revision:** left at the IDF default, "revisions < v3.0", which matches Olimex's production test.

## 5. Resource Budget

Measured with `idf.py size` (ESP-IDF v6.1-dev, `-Os`, debug log and asserts compiled in):

| | Flash code | Flash rodata | DIRAM `.data` | DIRAM `.bss` | DIRAM `.text` (IRAM) | Total image |
|---|---|---|---|---|---|---|
| **Baseline (whole image)** | 80,384 | 32,928 | 9,820 | 5,872 | 49,684 | 173,176 |
| **BMDS share** (`libbmds_*`, `libmain`) | 1,384 | 400 | 1 | 1 | 0 | ≈1.8 KB |

- DIRAM use is 65,376 of 576,464 bytes (11.3%). Almost all of it is IDF and FreeRTOS: the IRAM text is the flash-cache-disabled code paths, the ISRs, FreeRTOS, and the PSRAM driver.
- PSRAM support costs about 11.5 KB of image, 6.3 KB of it in IRAM.
- Turning off the debug log and asserts saves about 128 bytes of image.

Measured on hardware (boot report):

| | Value |
|---|---|
| Reset to `app_main()` | 254 ms. About 170 ms of it is IDF's PSRAM timing tuning; it was 1.54 s with the PSRAM memory test on. |
| BMDS init (banner, stages, report) | about 1 ms; the `memory` stage takes 63 µs |
| Heap free, `Internal` | 611,991 of 662,263 bytes. The largest block is 385,024. |
| Heap free, `InternalDma` | 572,379 of 621,535 bytes |
| Heap free, `Psram` / `PsramDma` | 33,551,868 of 33,554,432 bytes |
| Heap free, `Fast` (TCM) | 7,708 of 8,088 bytes |
| Main task stack | 2,288 bytes never used (of 3,584) |

## 6. ABI Impact

None. Everything here is kernel-internal. `Error` codes and `MemSpace` values are explicitly **not** ABI; the SDK will define its own stable constants.

## 7. Testing

- **Host tests** (`tests/host`, doctest vendored in `thirdparty/doctest`, v2.4.12):
  - Cover `Error`, `Result`, `StringView`, `Span`, and `MemSpace`.
  - Built with the same `-fno-exceptions -fno-rtti -Werror` policy; doctest runs in `DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS` mode.
  - Include compile-time (`static_assert`) checks of the constexpr APIs.
- **Target:**
  - Both `CONFIG_BMDS_DEBUG_LOG`/`CONFIG_BMDS_ASSERT` configurations build warning-free.
  - The `esp_err_t` constant checks run in every target build.
- **On hardware:** the boot report was checked: chip revision v1.3, 32 MB PSRAM, every MemSpace backed, all stages passing. The boot `memory` stage caught the missing PSRAM on the first flash.

## 8. Open Questions / Future Work

- **PSRAM timing tuning** (about 170 ms of boot): check whether it can be skipped or cached at 80 MHz.
- **`SPIRAM_USE_MALLOC`** (the IDF default): plain `malloc()` of 16 KB or more may land in PSRAM. Fine for IDF internals, but BMDS code always names a MemSpace. Revisit whether `SPIRAM_USE_CAPS_ALLOC` fits the philosophy better.
- **Boot banner:** IDF prints "GPIO 38 and 37 are used as console UART I/O pins" even when the console is USB-Serial/JTAG. It is cosmetic.
- **PSRAM speed:** move to 200 MHz (experimental in IDF) when framebuffer bandwidth matters (Phase 4). Measure it.
- **Optimization level:** `-Os` vs `-O2`. Decide with benchmarks once there are hot paths; hot functions can opt in individually.
- **SD card-detect polarity:** confirm on hardware.
- **Partition table:** OTA slots and a coredump partition, once there is a reason. A change requires reflashing the partition table but has no userspace impact.
- **`BMDS_ISR_ATTR`:** defined when the interrupt dispatcher lands.
