/**
 * @file      log.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Kernel logging.
 *
 * @code
 * #include <bmds/log.h>
 * BMDS_DEBUG(SdCard)            // once per file that uses bmdsDebug()
 *
 * bmdsInfo("SdCard(%p): mounted %s", this, path);
 * bmdsDebug("SdCard(%p): cmd %u arg 0x%08x", this, cmd, arg);
 * @endcode
 *
 * Format strings must be literals (checked at compile time, kept in flash).
 * Never log from an ISR or on a hot path's success path.
 *
 * Every line is kept in a history ring (`CONFIG_BMDS_LOG_HISTORY_BYTES`, in
 * PSRAM) and handed to the sink, normally the serial console. ESP-IDF's own
 * `ESP_LOG*` output is redirected through the same path.
 */

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/** @brief Log severity. */
enum class LogLevel : uint8_t {
        Debug, ///< Per-module diagnostics (`bmdsDebug`).
        Info,  ///< Normal operational events.
        Warn,  ///< Recoverable problems worth investigating.
        Err,   ///< Failed operations.
};

/**
 * @brief Descriptor of a per-module debug switch, created by `BMDS_DEBUG(Module)`.
 *
 * Descriptors are constant and live in a linker-collected rodata section
 * (no global constructors, no RAM), so the full set can be listed at
 * runtime. Only the one-byte enable flag they point to lives in RAM.
 */
struct alignas(4) DebugModule {
                const char *name;    ///< Module name (the `BMDS_DEBUG` argument).
                bool       *enabled; ///< The module's enable flag.

                /** @brief Returns true if `bmdsDebug()` output from this module is shown. */
                bool isEnabled() const { return *enabled; }

                /** @brief Turns this module's `bmdsDebug()` output on or off. */
                void setEnabled(bool on) const {
                        *enabled = on;
                        return;
                }
};
static_assert(sizeof(DebugModule) % alignof(DebugModule) == 0, "DebugModule must tile as an array");

/**
 * @brief Kernel log output.
 *
 * @par Thread Safety
 * Thread-safe after `init()`. Not ISR-safe.
 */
class Log {
        public:
                /**
                 * @brief Receives complete log text (one or more `'\n'`-terminated lines).
                 *
                 * Called with the log lock held, so lines arrive in order. Must
                 * not log, and should bound its blocking (drop output rather than
                 * stall the caller).
                 */
                using Sink = void (*)(const char *text, size_t lengthBytes);

                /**
                 * @brief Creates the log lock and history, and redirects ESP-IDF logging.
                 *
                 * Called first by `Boot::run()`. Before this, lines go straight to
                 * stdout without locking (only the boot task is running).
                 * @return `Error::Ok`, or `Error::NoMem` if the history could not
                 *         be allocated (logging still works, without history).
                 */
                static Error init();

                /**
                 * @brief Sets where log output goes; null restores stdout.
                 *
                 * The sink is called for lines written after this returns.
                 */
                static void setSink(Sink sink);

                /**
                 * @brief Writes one log line.
                 *
                 * Use the `bmdsDebug/Info/Warn/Err` macros instead of calling
                 * this directly. Lines longer than `CONFIG_BMDS_LOG_LINE_MAX`
                 * are truncated. Formats on the caller's stack; no allocation.
                 *
                 * @param level  Severity.
                 * @param module Debug module name, or null.
                 * @param file   Source file name.
                 * @param line   Source line.
                 * @param fmt    printf-style format string.
                 * @blocking Bounded: waits for the log lock and the sink (the serial
                 *           console drops output after a short timeout).
                 */
                static void write(LogLevel level, const char *module, const char *file, int line, const char *fmt, ...)
                        BMDS_PRINTF_FORMAT(5, 6);

                /** @brief `va_list` form of `write()`. */
                static void writeV(LogLevel level, const char *module, const char *file, int line, const char *fmt,
                                   va_list args);

                /**
                 * @brief Writes preformatted text to the history and the sink.
                 *
                 * For text that already carries its own line format, such as
                 * redirected ESP-IDF log lines.
                 */
                static void writeRaw(const char *text, size_t lengthBytes);

                /**
                 * @brief Copies the log history, oldest first.
                 *
                 * When the history has wrapped or @p dst is smaller than it, the
                 * copy starts at the first complete line that fits.
                 * @param dst Destination.
                 * @return The number of bytes copied.
                 */
                static size_t copyHistory(Span<char> dst);

                /** @brief Returns the history capacity in bytes (0 if there is none). */
                static size_t historyCapacity();

                /** @brief Returns the minimum level written by `bmdsInfo/Warn/Err`. */
                static LogLevel level() { return _level; }

                /** @brief Sets the minimum level written by `bmdsInfo/Warn/Err`. */
                static void setLevel(LogLevel level) {
                        _level = level;
                        return;
                }

                /**
                 * @brief Returns all registered debug modules.
                 *
                 * Empty when debug logging is compiled out
                 * (`CONFIG_BMDS_DEBUG_LOG=n`).
                 */
                static Span<const DebugModule> debugModules();

                /**
                 * @brief Finds a debug module by name (case-sensitive).
                 * @return The module, or null if there is none.
                 */
                static const DebugModule *findDebugModule(StringView name);

                /** @brief Never called; lets disabled `bmdsDebug()` still type-check its arguments. */
                static inline void formatCheck(const char *, ...) BMDS_PRINTF_FORMAT(1, 2) {}

        private:
                static constinit inline LogLevel _level = LogLevel::Info;
};

BMDS_NAMESPACE_END

/** @cond internal */
#define BMDS_LOG_IMPL(lvl, module, fmt, ...)                                                                           \
        do {                                                                                                           \
                if ((lvl) >= ::bmds::Log::level())                                                                     \
                        ::bmds::Log::write(lvl, module, BMDS_SOURCE_FILE, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__);   \
        } while (0)
/** @endcond */

/** @brief Logs an operational event. */
#define bmdsInfo(fmt, ...) BMDS_LOG_IMPL(::bmds::LogLevel::Info, nullptr, fmt __VA_OPT__(, ) __VA_ARGS__)

/** @brief Logs a recoverable problem. */
#define bmdsWarn(fmt, ...) BMDS_LOG_IMPL(::bmds::LogLevel::Warn, nullptr, fmt __VA_OPT__(, ) __VA_ARGS__)

/** @brief Logs a failed operation. */
#define bmdsErr(fmt, ...) BMDS_LOG_IMPL(::bmds::LogLevel::Err, nullptr, fmt __VA_OPT__(, ) __VA_ARGS__)

#if BMDS_ENABLE_DEBUG_LOG

/**
 * @def BMDS_DEBUG
 * @brief Registers the debug module for this source file. Place after the includes.
 */
#define BMDS_DEBUG(Name)                                                                                               \
        namespace {                                                                                                    \
                constinit bool                                                           bmdsDebugEnabled = false;     \
                [[gnu::used, gnu::section(".bmds_debug")]] constexpr ::bmds::DebugModule bmdsDebugModule = {           \
                        #Name, &bmdsDebugEnabled};                                                                     \
        }

/** @brief Logs a debug message if this file's module is enabled. Requires `BMDS_DEBUG(Module)`. */
#define bmdsDebug(fmt, ...)                                                                                            \
        do {                                                                                                           \
                if (bmdsDebugEnabled) [[unlikely]]                                                                     \
                        ::bmds::Log::write(::bmds::LogLevel::Debug, bmdsDebugModule.name, BMDS_SOURCE_FILE, __LINE__,  \
                                           fmt __VA_OPT__(, ) __VA_ARGS__);                                            \
        } while (0)

#else

#define BMDS_DEBUG(Name)
#define bmdsDebug(fmt, ...)                                                                                            \
        do {                                                                                                           \
                if (false) ::bmds::Log::formatCheck(fmt __VA_OPT__(, ) __VA_ARGS__);                                   \
        } while (0)

#endif
