/**
 * @file      findbar.h
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
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

class CheckBox;
class LineEdit;
class TextEdit;

/**
 * @brief Find, replace and go to a line in a `TextEdit` (`devplan/editor.md` §2): a strip a
 *        window shows under its editor, hidden until wanted.
 *
 * Find: the text, Match case, Whole word, Next (Enter, F3) and Previous (Shift+Enter,
 * Shift+F3), wrapping at the ends. Replace adds a row: the replacement, Replace (the one found,
 * then the next) and All (one undo step). Go to line: a line number, Enter goes. Escape closes
 * it and gives the editor the keys again. What happened shows at its right ("Not found",
 * "3 replaced").
 *
 * @par Thread Safety
 * As `Widget`.
 */
class FindBar : public BoxLayout {
        public:
                /** @brief What it is open for. */
                enum class Mode : uint8_t {
                        Find,
                        Replace,
                        GoToLine,
                };

                /** @brief Told when it opens or closes (its window lays out again). */
                using Toggled = void (*)(void *arg, FindBar &bar);

                explicit FindBar(Widget *parent);

                /** @brief Sets who is told when it opens or closes. */
                void setToggled(Toggled fn, void *arg) {
                        _toggled = fn;
                        _toggledArg = arg;
                        return;
                }

                /** @brief Sets the editor it works on (null: none). */
                void setTarget(TextEdit *target) { _target = target; }
                /** @brief Returns the editor it works on. */
                TextEdit *target() const { return _target; }

                /**
                 * @brief Shows it for @p mode, the field filled from the editor (its selection, if
                 *        on one line, for Find and Replace; the cursor's line for Go to line) and
                 *        given the keys.
                 */
                void open(Mode mode);
                /** @brief Hides it and gives the editor the keys. */
                void close();
                /** @brief Returns what it is open for. */
                Mode mode() const { return _mode; }

                /** @brief Finds the next (or previous) match of what it holds. @return True if found. */
                bool findNext(bool backward);

                bool keyBeforeChild(const KeyEvent &event) override;

        private:
                static void accepted(void *arg, LineEdit &edit);
                static void cancelled(void *arg, LineEdit &edit);
                static void clicked(void *arg, Button &button);
                static void edited(void *arg, LineEdit &edit);

                uint32_t flags() const;
                void     say(StringView text);
                void     replaceOne();
                void     replaceAll();
                void     goToLine();

                TextEdit  *_target = nullptr;
                Toggled    _toggled = nullptr;
                void      *_toggledArg = nullptr;
                Mode       _mode = Mode::Find;
                BoxLayout *_findRow = nullptr;
                BoxLayout *_replaceRow = nullptr;
                Label     *_findLabel = nullptr;
                LineEdit  *_find = nullptr;
                LineEdit  *_replace = nullptr;
                CheckBox  *_case = nullptr;
                CheckBox  *_word = nullptr;
                Button    *_next = nullptr;
                Button    *_previous = nullptr;
                Button    *_replaceButton = nullptr;
                Button    *_allButton = nullptr;
                Button    *_closeButton = nullptr;
                Label     *_status = nullptr;
};

BMDS_NAMESPACE_END
