/**
 * @file      screenconsole.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/bytering.h>
#include <bmds/config.h>
#include <bmds/console.h>
#include <bmds/memspace.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/terminalscreen.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A console whose terminal is a `TerminalScreen` in memory: what a
 *        terminal window shows (`devplan/windows.md` §4e).
 *
 * Output is applied to the screen (`\n` as CR LF when the mode says so);
 * input is what the owner pushes (`pushInput()`: typed keys), with the
 * screen's replies (status and size reports) added to it. A Ctrl-C (or
 * Ctrl-\) pushed while signals are on sends `SIGINT` (`SIGQUIT`) and drops
 * the input before it, as on a tty.
 *
 * Whoever draws the screen reads it through `Locked`. After output that
 * changed the screen or moved the cursor, the `changed` hook is called (on
 * the writer's task, no lock held) once, until the owner calls `drawn()`: a
 * burst of output asks for one redraw. When output changes the screen's
 * title (`OSC 0`, `OSC 2`), the `title` hook is called, likewise.
 *
 * @par Thread Safety
 * Thread-safe: writers, one reader, the input's producers and the renderer
 * may run on different tasks.
 */
class ScreenConsole final : public Console {
        public:
                /** @brief Called when the screen changed and wasn't drawn since. */
                using Changed = void (*)(void *arg);

                /** @brief Input bytes buffered. */
                static constexpr size_t InputBytes = 512;

                /** @brief A console named @p name (outliving it), its screen's cells in @p space. */
                ScreenConsole(const char *name, MemSpace space);

                /**
                 * @brief Sets the screen's size and the console's (`setSize()`).
                 * @return As `TerminalScreen::resize()`.
                 */
                Error resize(uint16_t columns, uint16_t rows);

                /** @brief Sets the hook called when the screen needs drawing. */
                void setChangedHook(Changed hook, void *arg);

                /**
                 * @brief Sets the hook called (on the writer's task, no lock held)
                 *        each time output changes the screen's title.
                 */
                void setTitleHook(Changed hook, void *arg);

                /** @brief The owner drew the screen: the next change calls the hook again. */
                void drawn() {
                        _asked.store(false, std::memory_order_release);
                        return;
                }

                /**
                 * @brief Adds @p data to the input (typed keys).
                 * @return The bytes taken (fewer when the input is full).
                 */
                size_t pushInput(ByteSpan data);

                /** @brief The screen, locked against writers while it lives. */
                class Locked {
                        public:
                                explicit Locked(ScreenConsole &console)
                                    : _console(console), _lock(console._lock, WaitForever) {}
                                Locked(const Locked &) = delete;
                                Locked &operator=(const Locked &) = delete;

                                /** @brief Returns the screen. */
                                TerminalScreen &screen() { return _console._screen; }

                        private:
                                ScreenConsole &_console;
                                Mutex::Locker  _lock;
                };

                const char    *name() const override { return _name; }
                uint32_t       capabilities() const override { return Ansi; }
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                Error          flush(unsigned int) override { return Error::Ok; }

                using Console::write;

        protected:
                Result<size_t> readInput(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> tryReadInput(WritableByteSpan buf) override;
                bool           hasInput() const override { return !_input.isEmpty(); }

        private:
                static void reply(void *context, StringView text);
                size_t      push(ByteSpan data); // with _inputLock held

                const char           *_name;
                Mutex                 _lock;      // the screen
                Mutex                 _inputLock; // the input's producers
                TerminalScreen        _screen;
                uint8_t               _inputStorage[InputBytes] = {};
                ByteRing              _input;
                Semaphore             _ready{1, 0};
                std::atomic<bool>     _flush{false}; // input before a Ctrl-C or Ctrl-\ to drop
                std::atomic<uint32_t> _flushTo{0};
                std::atomic<bool>     _asked{false}; // the hook called, not yet drawn
                Changed               _changed = nullptr;
                void                 *_changedArg = nullptr;
                Changed               _titleHook = nullptr;
                void                 *_titleArg = nullptr;
};

BMDS_NAMESPACE_END
