# Coding Standards

This document describes the coding conventions and design patterns used throughout BMDS. All new code should follow these standards. Existing code that deviates should be updated when it is being modified for other reasons.

These standards are derived from the [libpromeki coding standards](https://github.com/jthwho/libpromeki/blob/main/CODING_STANDARDS.md) and adapted for a resource-constrained, bare-metal-ish embedded target (ESP32-P4, FreeRTOS, no MMU). Where BMDS deliberately deviates from promeki, the section says so.

---

## The Two Prime Directives

Everything else in this document is subordinate to these two rules.

### 1. Do Not Break Userspace

Borrowed directly from Linux. Once a userspace interface has shipped, **it never changes in a way that breaks an existing binary or existing source**. If a program built against an older BMDS SDK stops working on a newer BMDS, that is a kernel bug, full stop — no matter how "wrong" the program was or how much cleaner the new interface is.

"Userspace interface" includes:

- The syscall table: entry order, signatures, and semantics.
- Every struct, enum value, constant, and flag in the public SDK headers (`sdk/include/`).
- The executable format and loader contract (entry point, startup state, argument passing, memory layout guarantees).
- Shell command names, arguments, and output formats that scripts may depend on.
- On-disk formats (config files, file layouts) that user programs or users rely on.

Rules:

- **Append only.** New syscalls get new slots at the end of the table. Existing slots are never reused, reordered, or removed. A deprecated syscall keeps working forever (it may be reimplemented in terms of a newer one).
- **Never change semantics.** If an existing call needs different behavior, add a new call (`bmds_foo2()` or a flags argument that defaults to the old behavior).
- **Extensible structs.** Every struct that crosses the userspace boundary begins with a `uint32_t size` field set by the caller. The kernel only reads/writes fields that fit within `size`, and zero-fills/ignores the rest. New fields are only ever appended.
- **Reserved bits stay reserved.** Flag arguments must reject unknown bits (`Error::Invalid`) so they can be given meaning later without silently changing old programs' behavior.
- **The ABI is plain C.** No C++ types, classes, references, templates, exceptions, or name-mangled symbols cross the boundary. See [Userspace SDK](#userspace-sdk-c).
- **Changes to `sdk/` require an ABI review.** Any diff touching the SDK headers or the syscall table must be called out explicitly in the commit message and checked against these rules.

See `docs/ABI.md` for the full ABI policy and versioning scheme.

### 2. The OS Gets Out of the Way

BMDS exists to let programs use the hardware. Every byte of RAM, every cycle, and every byte of IRAM the OS consumes is taken away from the user. Efficiency is a correctness requirement, not an optimization pass.

- **Measure, don't guess.** Track RAM, IRAM, flash, and CPU cost of every subsystem. `idf.py size` / `idf.py size-components` deltas are reviewed for any non-trivial change. A subsystem's devplan document records its resource budget.
- **Zero cost when unused.** Features that are not in use must cost nothing at runtime (no polling, no timers, no resident buffers). Optional subsystems are compile-time selectable via Kconfig.
- **Never poll when you can wait.** Use interrupts, task notifications, and DMA. An idle system should be in `WFI` / low-power idle, not spinning.
- **Static over dynamic.** Kernel data structures are allocated at boot or are statically sized pools. Heap allocation at runtime in the kernel is the exception and must be justified.
- **No allocation in ISRs or hot paths.** Ever. See [Execution Context](#execution-context-isr-safety-and-blocking).
- **Prefer `constexpr` and rodata over static initializers.** Tables, descriptors, and registries should live in flash (rodata) and be built at compile time. Avoid global constructors entirely in the kernel; they cost boot time, RAM, and introduce initialization-order bugs.
- **Avoid copies.** Use move semantics, views (`StringView`, `Span`), and DMA-capable buffers handed off by ownership instead of memcpy.
- **Watch code size.** Templates and inlining are tools, not defaults — every template instantiation costs flash and possibly IRAM. Prefer a small non-template core with a thin template wrapper when a template would otherwise be instantiated for many types.
- **IRAM is precious.** Only code that must run with the flash cache disabled, or ISR code, goes in IRAM.

---

## Design Philosophy

BMDS follows a **Qt-inspired** design: friendly, readable, consistently-named APIs over raw `std::` or ESP-IDF calls, while retaining interoperability. Key principles:

- **Value semantics**: Data classes are plain value types. The layer above decides whether to use `MyClass::Ptr` (shared) or `MyClass::UPtr` (unique) ownership.
- **No raw ownership**: Memory is managed via `SharedPtr`, `UniquePtr`, `Buffer`, or RAII. Raw pointers are only used for non-owning references. See [Heap Ownership and Pointer Types](#heap-ownership-and-pointer-types).
- **Error codes, never exceptions**: Exceptions are disabled (`-fno-exceptions`). Use the `Error` class and return values. Constructors never fail in a way that needs reporting — use a factory or an `open()`/`init()` method that returns `Error` instead.
- **No RTTI (for now)**: Built with `-fno-rtti`. No `dynamic_cast`, no `typeid`. Use explicit type tags or virtual methods where type discovery is needed. This may be revisited, but code must not assume RTTI exists.
- **Minimal abstraction**: Wrap `std::` / FreeRTOS / IDF types to improve ergonomics and to enforce our rules (memory space, timeouts, context safety), but don't add layers of indirection for their own sake.
- **Fix the library, don't work around it**: When a BMDS type has a missing operator, conversion, or ergonomic gap that forces awkward call sites, fix the type rather than scattering workarounds.
- **Hardware truth lives in one place**: Pin assignments, peripheral instances, memory regions, clock settings, and board capabilities live in the board definition (see [Hardware Abstraction](#hardware-abstraction-and-memory-spaces)). No GPIO numbers or peripheral IDs scattered through drivers.

---

## Execution Context, ISR Safety, and Blocking {#execution-context-isr-safety-and-blocking}

Every function in BMDS has an execution context contract. The contract is encoded in the **name** (so it's visible at the call site) and in the **Doxygen** (so it's visible in the docs).

### The Contexts

| Context | Name pattern | May block? | May allocate? | May log? | Callable from ISR? |
|---|---|---|---|---|---|
| **ISR-safe** | `fooFromIsr()` | Never | Never | Trace only | Yes |
| **Non-blocking** | `tryFoo()` or a plain name documented `@nonblocking` | Never | Only if documented | Yes | No |
| **Blocking** | `foo(unsigned int timeoutMs)` | Up to `timeoutMs` | Yes | Yes | No |

### Blocking Calls

The preferred design is that nothing blocks. When blocking is unavoidable:

- Any function that can block **must** take a `timeoutMs` parameter (`unsigned int`, milliseconds).
- `timeoutMs == 0` means **wait forever**. This is consistent with promeki, but it is heavily discouraged in BMDS.
- **Deviation from promeki:** blocking functions have **no default argument** for `timeoutMs`. Callers must state the timeout explicitly. To wait forever, pass the named constant `bmds::WaitForever` (which equals `0`) — never a literal `0`. This keeps every infinite wait greppable and reviewable.
- On timeout, return `Error::Timeout`.
- Blocking calls must document what they wait on.

```cpp
static constexpr unsigned int WaitForever = 0;

Result<Event> waitForEvent(unsigned int timeoutMs);

auto [ev, err] = queue.waitForEvent(100);          // good
auto [ev, err] = queue.waitForEvent(WaitForever);  // allowed, but justify it
auto [ev, err] = queue.waitForEvent(0);            // wrong: use WaitForever
```

### Non-Blocking Calls

- A `tryFoo()` variant returns immediately. If it cannot complete, it returns `Error::WouldBlock` (or a more specific code such as `Error::Busy` / `Error::Full` / `Error::Empty`).
- When a class offers a blocking `foo(timeoutMs)`, it should also offer `tryFoo()` if a non-blocking form makes sense.
- Functions that are inherently non-blocking (getters, pure computation) don't need a `try` prefix; they are documented `@nonblocking` only when it isn't obvious.

### ISR-Safe Calls

- Named with the `FromIsr` suffix (mirroring FreeRTOS's `FromISR`, but in our camelCase).
- Never block, never allocate, never take a mutex, never call a non-ISR-safe function, never call `printf`/logging (use the ISR trace facility, once it exists).
- Must be placed in IRAM when they can run while the flash cache is disabled; mark with `BMDS_ISR_ATTR` (wraps `IRAM_ATTR`) rather than using `IRAM_ATTR` directly.
- Return `Error` (typically `Ok`, `Full`, `WouldBlock`).
- Context switching on exit is handled by the BMDS interrupt dispatcher, not by each `FromIsr` call. `FromIsr` functions record that a higher-priority task was woken; the dispatcher performs a single yield at the end of the ISR. (This keeps call sites free of `BaseType_t *woken` plumbing.)

### Hot Paths

Code designated a **hot path** (per-pixel/per-sample loops, audio/video callbacks, packet paths, the scheduler tick, input dispatch) follows the ISR rules for allocation and locking even if it does not run in an ISR: no heap, no blocking, no logging on the success path, no virtual dispatch per element. Mark such functions `@hotpath` in Doxygen.

### Documentation Tags

Doxygen aliases (defined in the Doxyfile) are used on every public function where context matters:

- `@isrsafe` — may be called from an ISR.
- `@nonblocking` — never blocks; safe from any task.
- `@blocking` — may block up to `timeoutMs`.
- `@hotpath` — follows hot path rules.

---

## Object Categories

BMDS classes fall into three categories with different ownership, copying, and lifetime rules. Ask: "Does this class represent a *value*, a *thing*, or *infrastructure*?"

### Data Objects

Values or containers of values — the majority of classes. Copyable, no identity, not internally thread-safe.

Exception: **allocating containers (`String`, `List<T>`) are move-only.** A copy would allocate and could fail, and constructors can't report failure. Copy explicitly with `clone()`, which returns `Result<T>`. Every operation that may allocate returns `Error` (`NoMem`, leaving the object unchanged).

| Category | Storage | SharedPtr support | Examples |
|---|---|---|---|
| **Simple** | Direct members | None | `Point`, `Size2Du32`, `Rect2Di32`, `Rational`, `FourCC`, `Duration`, `Color` |
| **Shareable** | Direct members | `BMDS_SHARED_FINAL` + `using Ptr = SharedPtr<T>` | `String`, `Buffer`, `List<T>`, `Map<K,V>`, `Image` |

Data objects are shared across tasks by **handoff**: copy simple types, or hand a `Ptr` to the consuming task and stop mutating it. Do not wrap data objects in a mutex as a general practice.

### Functional Objects (ObjectBase)

Active entities with identity: drivers, devices, consoles, shell sessions, processes. They derive from `ObjectBase`.

- **Not copyable.** Each instance is unique.
- **Parent/child ownership.** Destroying a parent destroys its children. Raw pointers are used for non-owning references within the tree.
- **Signals and slots** for communication between functional objects. Signal/slot delivery must not allocate on the emit path (connections are allocated at connect time; queued cross-task delivery uses preallocated queues).
- **No SharedPtr.** Never manage an `ObjectBase` with `SharedPtr`.

### Utility Classes

Infrastructure: `Mutex`, `SpinLock`, `Semaphore`, `Queue<T>`, `Task`, `Atomic<T>`, `EventGroup`. They wrap FreeRTOS primitives.

- **Not copyable, not movable.**
- Usually thread-safe; document it.
- Synchronization classes provide nested RAII lockers (`Mutex::Locker`, `SpinLock::Locker`).
- **Prefer static allocation.** Utility classes that wrap FreeRTOS objects use the `xFooCreateStatic` APIs with storage embedded in the object, so constructing one never touches the heap.

---

## Hardware Abstraction and Memory Spaces

### Board Definitions

All hardware truth for a board lives in one place: `components/bmds_board/boards/<board>/`. This includes pin assignments, peripheral instances, bus speeds, memory region sizes, which devices exist, and their capabilities. Drivers receive their configuration from the board definition — they never hard-code a GPIO number, peripheral index, or chip-specific constant.

Board definitions are `constexpr` data (rodata), selected at build time via Kconfig. Porting BMDS to a new board should mean writing a new board definition, not editing drivers.

### MemSpace

Every allocation comes from an explicit **memory space**. The ESP32-P4 has several kinds of memory with very different characteristics (fast internal SRAM, TCM, DMA-capable regions, large-but-slower PSRAM, LP SRAM), and choosing wrongly is a common source of both bugs (DMA from non-DMA memory, cache coherency) and waste (burning internal SRAM on bulk data).

- `MemSpace` identifies a kind of memory (e.g. `MemSpace::Internal`, `MemSpace::InternalDma`, `MemSpace::Psram`, `MemSpace::PsramDma`, `MemSpace::Fast`). The set of spaces and their mapping to physical regions/heap caps is defined by the board/chip layer.
- APIs that allocate either take a `MemSpace` parameter or document which space they use.
- Bulk data (framebuffers, file buffers, user program images) defaults to PSRAM. Internal SRAM is reserved for things that need it (ISR data, DMA descriptors, hot data, stacks of latency-critical tasks).
- `Buffer` records its `MemSpace` and alignment, and handles cache writeback/invalidate for DMA when the space requires it.

---

## File Layout

### Header Files

```cpp
/**
 * @file      filename.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/dependency.h>

BMDS_NAMESPACE_BEGIN

// Forward declarations

class MyClass {
        public:
                // Types / enums / ownership aliases
                // Static constants
                // Static methods
                // Constructors / destructor
                // Operators
                // Const accessors (getters)
                // Mutators (setters)
                // Other methods

        protected:
                // ...

        private:
                // Members
};

BMDS_NAMESPACE_END
```

### File Header Comment

Every source file starts with a Doxygen `@file` block with `@copyright Jason Howard. All rights reserved.`, an `SPDX-License-Identifier: MIT` line, and `See LICENSE file in the project root folder for license information.` The `@author` tag is optional.

### Include Guards

Always `#pragma once`. No `#ifndef` guards.

### Includes

Order:

1. C/C++ standard library headers
2. ESP-IDF / FreeRTOS / third-party headers
3. BMDS headers (`<bmds/foo.h>`)

Use angle brackets for all includes: `<bmds/foo.h>`, not `"bmds/foo.h"`.

**Keep IDF headers out of public BMDS headers** where possible. Forward declare, or use opaque storage, so that the bulk of BMDS (and especially `bmds_core`) compiles on the host for unit testing.

### Subsystem Feature Guards

Headers tied to an optional subsystem wrap their body in `#if BMDS_ENABLE_<SUBSYSTEM>` ... `#endif` (after `#pragma once` and `#include <bmds/config.h>`). `BMDS_ENABLE_*` values come from Kconfig via `bmds/config.h`. Source files don't need the guard; CMake only builds them when the subsystem is enabled.

---

## Naming Conventions

### Namespace and Macros

- All C++ code lives in the `bmds` namespace, opened with `BMDS_NAMESPACE_BEGIN` / `BMDS_NAMESPACE_END`.
- Never `using namespace bmds;` in headers. Never `using namespace std;` anywhere.
- Macros are all caps with a `BMDS_` prefix: `BMDS_SHARED_FINAL`, `BMDS_OBJECT`, `BMDS_SIGNAL`, `BMDS_ASSERT`, `BMDS_ISR_ATTR`.

### Classes and Types

- **PascalCase**: `String`, `Duration`, `BlockDevice`, `ObjectBase`.
- Type aliases are PascalCase: `using Iterator = ...;`.
- Templates are never spelled directly at call sites — define a `using` alias (`Buffer::Ptr`, not `SharedPtr<Buffer>`).

### Ownership Aliases

Every heap-managed class defines its aliases at the top of `public:`:

- `using Ptr = SharedPtr<MyClass>;` — shared ownership (classes with `BMDS_SHARED_FINAL`).
- `using UPtr = UniquePtr<MyClass>;` — unique ownership.
- `using List = bmds::List<MyClass>;` — plain value list.
- `using PtrList = bmds::List<Ptr>;` — list of shared pointers.
- `using ImplPtr = UniquePtr<Impl>;` — next to a forward-declared PIMPL `Impl`.

### Numeric Template Aliases

Numeric templates get explicitly-suffixed aliases at namespace scope in the template's header — no unsuffixed defaults: `i8 u8 i16 u16 i32 u32 i64 u64 f d` (e.g. `Point2Di32`, `Size2Du32`, `Rect2Di16`). Dimensionality is part of the base name; the type suffix comes last. Coordinates are typically signed, sizes unsigned.

### Methods

- **camelCase** for all methods.
- **Getters**: bare noun, no `get` prefix — `width()`, `size()`, `mode()`.
- **Setters**: `set` prefix — `setWidth()`.
- **Predicates**: `is` prefix — `isValid()`, `isEmpty()`.
- **Presence**: `has` prefix — `hasParent()`.
- **Counts**: `*Count()` suffix — `sectorCount()`.
- **Conversions**: `to*()` — `toString()`.
- **Static factories**: `from*()` or descriptive — `fromString()`, `now()`.
- **Context suffixes/prefixes**: `fromIsr` suffix, `try` prefix — see [Execution Context](#execution-context-isr-safety-and-blocking).

### Members, Enums, Constants

- Private members: underscore prefix — `_width`, `_parent`.
- Enum values: PascalCase — `Ok`, `Invalid`, `NotExist`. Enums are `enum class` or nested in their owning class.
- Constants: `static constexpr` in the class, PascalCase — `DefaultAlign`, `SectorSize`.

### Units in Names

When a value has a unit and the type does not carry it, put the unit in the name: `timeoutMs`, `delayUs`, `sizeBytes`, `rateHz`. Prefer a typed `Duration` in new APIs where practical.

### Reserved Identifiers

Never use leading double underscores or underscore-capital identifiers. Use a `Detail` namespace or `Impl` suffix.

---

## Standard Library and Wrapper Types

### Prefer BMDS Wrappers

When a BMDS wrapper exists for a `std::` or FreeRTOS type, use the wrapper — even in private members and implementation files. The wrappers exist to enforce memory space, context, and timeout rules.

| Raw type | BMDS wrapper |
|---|---|
| `std::string` | `String` |
| `std::string_view` | `StringView` |
| `std::vector<T>` | `List<T>` |
| `std::map<K,V>` | `Map<K,V>` |
| `std::span<T>` | `Span<T>` |
| `std::unique_ptr<T>` | `UniquePtr<T>` |
| `std::shared_ptr<T>` | `SharedPtr<T>` |
| `std::atomic<T>` | `Atomic<T>` |
| `std::pair<T, Error>` | `Result<T>` |
| `SemaphoreHandle_t` (mutex) | `Mutex` |
| `portMUX_TYPE` | `SpinLock` |
| `QueueHandle_t` | `Queue<T>` |
| `TaskHandle_t` | `Task` |

When a `std::` type appears in a public API and no wrapper exists, alias it inside the class so callers don't spell `std::`.

### Restricted Standard Library Usage

The kernel must have predictable allocation and code size. In kernel code:

- **Allowed freely**: freestanding / header-only facilities — `<cstdint>`, `<cstddef>`, `<type_traits>`, `<utility>`, `<limits>`, `<bit>`, `<array>`, `<span>`, `<string_view>`, `<optional>`, `<algorithm>` (non-allocating algorithms), `<atomic>`, `<concepts>`, `<new>` (placement new).
- **Allowed via wrappers only**: allocating containers (`<vector>`, `<string>`, `<map>`) — through `List`, `String`, `Map`, which route allocation to a `MemSpace`.
- **Forbidden in the kernel**: `<iostream>` and streams, `<regex>`, `<locale>`, `<thread>`, `<mutex>`, `<condition_variable>`, `<future>` (use BMDS/FreeRTOS wrappers), `<functional>`'s `std::function` on any hot path (it may allocate — use a non-allocating `FunctionRef` or fixed-capacity callable).

---

## Heap Ownership and Pointer Types {#heap-ownership-and-pointer-types}

No raw `new`/`delete` for object lifetime.

1. **`SharedPtr<T>`** — shared, atomically reference-counted ownership with optional copy-on-write. For shareable data objects and cross-task handoff.
2. **`UniquePtr<T>`** — exclusive, move-only ownership. For PIMPL, owned resources, pipeline handoff. Prefer this when either works — it's cheaper.
3. **Raw pointers** — non-owning references only (parent back-pointers, observers, short-lived access).

Rules carried over from promeki:

- If you write `delete _member;` in a destructor, use `UniquePtr` instead.
- **Dual-pointer pattern** for conditional ownership: a non-owning `T *_x` for access plus a `T::UPtr _ownedX` populated only when owned.
- PIMPL classes declare their destructor in the header and define it (`= default`) in the `.cpp` where `Impl` is complete.
- `ObjectBase` children stay on raw pointers; the parent owns them.
- `ptr()` asserts non-null; `get()` (UniquePtr) may return null; use `isValid()` / `isNull()` before dereferencing when null is possible.

Additional embedded rules:

- **Every heap allocation has a MemSpace**, explicit or documented.
- **Kernel allocations happen at init time** wherever possible. Runtime allocation in the kernel needs a reason, and must handle `Error::NoMem` gracefully — out-of-memory is a normal, recoverable condition here, not a crash.
- **Fixed pools** are preferred for frequently allocated kernel objects (file handles, packets, events).

---

## Error Handling

### The Error Class

`Error` is a lightweight enum-based class (fits in a register) with codes such as `Ok`, `Invalid`, `NoMem`, `Timeout`, `WouldBlock`, `Busy`, `NotExist`, `Exists`, `NotSupported`, `IoError`, `NoSpace`, `Full`, `Empty`.

- `isOk()` / `isError()` for checks.
- `Error` and `Result<T>` are `[[nodiscard]]`: ignoring a returned error is a compile error. Discard explicitly with `(void)` only when that is genuinely correct.
- `Result<T>` is built with `makeResult(value)` / `makeError<T>(err)`; there is intentionally no implicit `Error` → `Result<T>` conversion (an unscoped `Error::Code` would silently convert to an integral `T`).
- `name()` / `desc()` for text (strings live in rodata).
- `Error::fromEspErr(esp_err_t)` and `Error::syserr()` (from `errno`) to translate lower-layer errors.

### Return Patterns

In order of preference:

1. **`Result<T>`** — value plus error, supports structured bindings:
   ```cpp
   static Result<Path> fromString(StringView str);
   auto [path, err] = Path::fromString(str);
   ```
2. **`Error`** — for operations that produce no value:
   ```cpp
   Error mount(const Path &mountPoint);
   ```
3. **`Error *` out-parameter** — for conversions where detail is optional:
   ```cpp
   int toInt(Error *err = nullptr) const;
   ```

Don't mix patterns within one class without a good reason. **Do not use `bool` to report success/failure** — only pure predicates return `bool`.

### Assertions

`BMDS_ASSERT(cond)` is for programmer errors (violated invariants), not runtime conditions. Out-of-memory, I/O failure, bad user input, and bad user-program syscall arguments are **runtime conditions** and must return an `Error`. A user program passing garbage to a syscall must never be able to trip a kernel assert.

---

## Formatting

`.clang-format` in the repository root is authoritative. Run it on changed code. In summary:

- **Indentation**: 8 columns, spaces (no tab characters).
- **Braces**: attached (K&R) for everything.
- **Access specifiers**: indented one level (8) from `class`; members two levels (16).
- **Column limit**: 120.
- **Pointer/reference alignment**: right (`char *p`, `const String &s`).
- **Alignment**: align related member declarations for readability.
- **Single-line bodies**: short getters/setters may be one line.
- **Void functions** end with an explicit `return;` on its own line (Qt convention).

---

## Documentation (Doxygen)

All public API is documented with `/** ... */` Doxygen comments.

- Every class: `@brief`, a longer description where helpful, and a **thread-safety statement** (one of: thread-safe; not thread-safe; conditionally thread-safe — distinct instances may be used concurrently; thread-affine — must be used from its owning task).
- Every public method: `@brief`, `@param` (with `[out]` / `[in,out]` as needed), `@return` (listing possible `Error` codes), `@tparam`.
- Context tags: `@isrsafe`, `@nonblocking`, `@blocking`, `@hotpath` as applicable.
- Memory: document which `MemSpace` an allocating API uses.
- Private members and implementation details don't need Doxygen; plain `//` comments are fine.

---

## Logging

All kernel logging goes through the BMDS logger macros, which sit on top of (or replace) ESP-IDF's `ESP_LOG*`.

| Macro | When |
|---|---|
| `bmdsDebug(fmt, ...)` | Per-module diagnostics. Compiled out in release builds; enabled per-module at runtime. Use liberally. |
| `bmdsInfo(fmt, ...)` | Normal operational events (boot stages, device attach/detach, mounts). |
| `bmdsWarn(fmt, ...)` | Recoverable, worth investigating. |
| `bmdsErr(fmt, ...)` | Failed operations / correctness problems. |

- Files that use `bmdsDebug()` register a module after the includes: `BMDS_DEBUG(SdCard)`. Registration costs no constructor: the module descriptor is placed in rodata by the linker and only a one-byte enable flag lives in RAM.
- **Capture all relevant state** in the message — paths, error descriptions, sizes, IDs.
- **Identify the instance**: `ClassName(%p): message` (add the name if the object has one: `Task(%p 'shell'): started`).
- **Never log from an ISR** or on a hot path's success path.
- Format strings must be literals (enables compile-time format checking and keeps strings in flash).

---

## Testing

Tests use [doctest](https://github.com/doctest/doctest).

- **Host tests** (`tests/host/`): for hardware-independent code (`bmds_core` and other pure logic). Built and run on the development machine — fast iteration, no board needed. This is why `bmds_core` must not depend on ESP-IDF headers.
- **Target tests** (`tests/target/`): for drivers and kernel code that needs the hardware; run on the board.
- Test files are named after what they test (`tests/host/string.cpp`). `TEST_CASE` per class or logical group, `SUBCASE` per behavior.
- Prefer `CHECK`; use `REQUIRE` only when later checks depend on it.
- **Quiet by default**: a passing run prints nothing but the summary. Use `INFO()` for context that only shows on failure; avoid `MESSAGE()`.
- Every bug fix comes with a test that would have caught it, where practical.
- **ABI tests**: the syscall table layout and SDK struct layouts are locked by tests (offsets, sizes, slot numbers). These tests must never be edited to make them pass — if one fails, the change broke userspace.

---

## Userspace SDK (C) {#userspace-sdk-c}

The userspace ABI and SDK headers (`sdk/include/bmds/`) are **C**, not C++, so that:

- The ABI is stable and has no name mangling or C++ runtime dependencies.
- Any compiler can target BMDS — including small compilers that we may eventually self-host on the device.

Conventions for SDK headers:

- **C99-compatible** syntax (no C23-only features) so the simplest compilers can parse them. Fixed-width types from `<stdint.h>`.
- Functions: `bmds_` prefix, snake_case — `bmds_file_open()`.
- Types: `bmds_` prefix, snake_case, `_t` suffix — `bmds_file_t`, `bmds_stat_t`.
- Macros/constants: `BMDS_` prefix, upper snake case — `BMDS_O_READ`.
- Every boundary-crossing struct starts with `uint32_t size`.
- Explicit-width integer fields only; no `int`/`long`/`bool`/enums of implementation-defined size in boundary structs. Enumerations are `#define`d constants or documented to be passed as `uint32_t`.
- No bitfields in boundary structs.
- Optional header-only C++ convenience wrappers may be provided on top, but the ABI itself is C.

Kernel-internal C code (rare — mostly glue) may use the latest C standard the toolchain supports (C23 / `gnu23`).

---

## Miscellaneous

- **C++ standard**: C++20 (later features may be adopted when the ESP-IDF toolchain supports them well).
- **C standard**: C23 for kernel C code; C99-compatible for SDK headers.
- **Build system**: ESP-IDF (CMake); host tests use plain CMake.
- **Compiler flags**: `-fno-exceptions -fno-rtti`; warnings are treated as errors in BMDS components.
- **Forward declare** in headers to minimize include chains (compile time and host-testability).
- **Host-testable kernel code.** `bmds_core` never includes ESP-IDF headers. In `bmds_kernel`, the synchronization classes (`Mutex`, `Semaphore`, `Deadline`), `Interrupt` and the stream layer keep FreeRTOS-free headers (opaque storage, sizes checked by `static_assert`), so code built on them also builds for the host tests. Their platform code lives in `src/` for the target and `src/host/` for the host, behind `bmds/detail/platform.h`.
- **No global constructors** in the kernel. Use `constexpr`, rodata, or explicit init functions called from the boot sequence. A kernel singleton that needs runtime construction (it holds a `Mutex`, say) lives in a `constinit StaticObject<T>` and is built by `create()` from its boot stage.
- **Explicit `return;`** at the end of void functions.
