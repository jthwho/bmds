/**
 * @file      lineeditor.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

class Console;

/** @brief Receives completion candidates (see `LineCompleter`). */
class CompletionSink {
        public:
                /** @brief Offers one candidate: the whole word it would complete to. */
                virtual void add(StringView candidate) = 0;

        protected:
                ~CompletionSink() = default;
};

/**
 * @brief Supplies Tab completions to a `LineEditor`.
 *
 * Candidates go to a sink as they are found, so the editor allocates
 * nothing; it may ask more than once for one key press (to find the common
 * prefix, then to list).
 */
class LineCompleter {
        public:
                /**
                 * @brief Calls `sink.add()` for each completion of the word that
                 *        ends at the cursor.
                 * @param line      The line up to the cursor.
                 * @param wordStart Where in @p line the word starts.
                 * @param sink      Where candidates go. Ones that don't start with the
                 *                  word are ignored.
                 */
                virtual void complete(StringView line, size_t wordStart, CompletionSink &sink) = 0;

        protected:
                ~LineCompleter() = default;
};

/**
 * @brief An interactive line editor with history, driven one input byte at a time.
 *
 * The editor is a pure state machine: the owner reads bytes from the
 * console and passes each to `feed()`. The editor echoes to the console
 * with ANSI cursor sequences and reports when a line is complete. It
 * allocates nothing; all state is inline (about 1.6 KB).
 *
 * Keys: printable ASCII inserts at the cursor. Supported editing keys:
 * - Enter (CR, LF, or CR LF);
 * - Backspace (DEL or BS) and Delete (`ESC[3~`, or Ctrl-D on a non-empty line);
 * - ←/→ (also Ctrl-B/F), and Ctrl-←/→ or Alt-B/F for whole words;
 * - Home/End (also Ctrl-A/E), ↑/↓ for history (also Ctrl-P/N);
 * - Ctrl-K, Ctrl-U and Ctrl-W kill to the end, to the start, and the previous word (Alt-Backspace also kills a word);
 * - Ctrl-C cancels the line, Ctrl-L clears the screen.
 * Ctrl-D on an empty line reports end of input. Other control bytes and
 * non-ASCII bytes are ignored.
 *
 * Echo uses relative cursor movement, so the prompt width doesn't need to
 * be known. Lines longer than the terminal width are not handled specially
 * yet (editing in the middle of a wrapped line misplaces the display).
 *
 * @par Thread Safety
 * Thread-affine: use from the console's reader task.
 */
class LineEditor {
        public:
                /** @brief Longest line, in characters. */
                static constexpr size_t LineMax = 255;

                /** @brief History storage (NUL-separated entries, oldest evicted first). */
                static constexpr size_t HistoryBytes = 1024;

                /** @brief Timeout for echo writes. */
                static constexpr unsigned int OutputTimeoutMs = 100;

                /** @brief What `feed()` reports. */
                enum class Event : uint8_t {
                        None,   ///< Keep feeding.
                        Line,   ///< A line is complete: see `line()`.
                        Cancel, ///< The line was abandoned (Ctrl-C).
                        Eof,    ///< End of input (Ctrl-D on an empty line).
                };

                /** @brief Creates an editor that echoes to @p console. */
                explicit LineEditor(Console &console);

                LineEditor(const LineEditor &) = delete;
                LineEditor &operator=(const LineEditor &) = delete;

                /**
                 * @brief Starts a new line: prints @p prompt and clears the line.
                 * @param prompt Prompt text; must stay valid until the next `start()`.
                 */
                void start(StringView prompt);

                /**
                 * @brief Processes one input byte.
                 * @return `Event::Line` when Enter completes a line, and so on.
                 */
                Event feed(char c);

                /** @brief Returns the current line. NUL-terminated. */
                StringView line() const { return StringView(_line, _length); }

                /**
                 * @brief Returns the line buffer, NUL-terminated, for in-place parsing.
                 *
                 * Valid after `Event::Line` until the next `start()`.
                 */
                char *lineData() { return _line; }

                /** @brief Returns the cursor position within the line. */
                size_t cursor() const { return _cursor; }

                /** @brief Returns the number of history entries. */
                size_t historyCount() const;

                /** @brief Returns history entry @p index (0 = newest), or an empty view. */
                StringView historyEntry(size_t index) const;

                /**
                 * @brief Adds @p text to the history.
                 *
                 * Empty text and repeats of the newest entry are skipped. Enter
                 * does this automatically.
                 */
                void addHistory(StringView text);

                /** @brief Reprints the prompt and line, e.g. after unrelated output. */
                void redraw();

                /**
                 * @brief Enables Tab completion through @p completer (null: off).
                 *
                 * Tab inserts what all candidates share; a single candidate is
                 * completed and followed by a space; when nothing can be added,
                 * the candidates are listed and the line reprinted.
                 */
                void setCompleter(LineCompleter *completer) {
                        _completer = completer;
                        return;
                }

        private:
                enum class InputState : uint8_t {
                        Normal,
                        Esc,
                        Csi,
                        Ss3
                };

                class Output;

                Event  handleControl(char c, Output &out);
                void   handleSequence(char final, Output &out);
                void   insert(char c, Output &out);
                void   eraseRange(size_t from, size_t to, Output &out);
                void   moveTo(size_t pos, Output &out);
                void   refreshFrom(size_t from, Output &out);
                void   loadLine(StringView text, Output &out);
                void   historyUp(Output &out);
                void   historyDown(Output &out);
                size_t wordLeft() const;
                void   complete(Output &out);
                size_t wordRight() const;

                Console       &_console;
                LineCompleter *_completer = nullptr;
                StringView     _prompt;
                size_t         _length = 0;
                size_t         _cursor = 0;
                size_t         _screenCursor = 0; // where the terminal cursor is, relative to the line start
                size_t         _savedLength = 0;
                size_t         _historyUsed = 0;
                size_t         _historyPos = 0; // 0 = editing a new line; n = showing the n-th newest entry
                unsigned       _params[2] = {};
                uint8_t        _paramCount = 0;
                InputState     _state = InputState::Normal;
                bool           _lastWasCr = false;
                char           _line[LineMax + 1] = {};
                char           _saved[LineMax + 1] = {}; // the new line, kept while browsing history
                char           _history[HistoryBytes] = {};
};

BMDS_NAMESPACE_END
