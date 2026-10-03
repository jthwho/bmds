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
 */

#pragma once

#include <cstdarg>
#include <cstdint>
#include <bmds/config.h>
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
 * Thread-safe. Not ISR-safe.
 */
class Log {
        public:
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
                 * @nonblocking (apart from the console output lock)
                 */
                static void write(LogLevel level, const char *module, const char *file, int line, const char *fmt, ...)
                        BMDS_PRINTF_FORMAT(5, 6);

                /** @brief `va_list` form of `write()`. */
                static void writeV(LogLevel level, const char *module, const char *file, int line, const char *fmt,
                                   va_list args);

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
