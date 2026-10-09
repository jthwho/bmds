/**
 * @file      combobox.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/stringview.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

class Menu;

/**
 * @brief A choice of one item from a list (`devplan/mediaplayer.md` §6d): a
 *        field showing the chosen item and an arrow; a press opens a `Menu` of
 *        the items under it.
 *
 * Up and Down (Left and Right) choose the previous and next item, Home and End
 * the ends, the wheel steps; Alt+Down, Alt+Up or F4 opens the menu. It takes
 * the focus. Each item may carry a value of the owner's (`itemData()`).
 *
 * @par Thread Safety
 * As `Widget`.
 */
class ComboBox : public Widget {
        public:
                /** @brief Told the item the user chose, when it changed (on the loop's thread). */
                using Changed = void (*)(void *arg, ComboBox &box, int index);

                static constexpr size_t   MaxItems = 32;
                static constexpr size_t   MaxText = 47;
                static constexpr uint16_t ArrowWidth = 16; ///< The arrow's button.

                explicit ComboBox(Widget *parent);
                ~ComboBox() override;

                /**
                 * @brief Adds an item (cut at `MaxText` bytes on a character) carrying
                 *        @p data; the first becomes current.
                 * @return `Ok` or `Full`.
                 */
                Error addItem(StringView text, uintptr_t data = 0);

                /** @brief Removes every item. */
                void clear();

                /** @brief Returns the items. */
                size_t count() const { return _count; }

                /** @brief Returns item @p index's text (empty past the end). */
                StringView itemText(size_t index) const {
                        return index < _count ? StringView(_items[index].text) : StringView();
                }

                /** @brief Returns item @p index's value (0 past the end). */
                uintptr_t itemData(size_t index) const { return index < _count ? _items[index].data : 0; }

                /** @brief Returns the current item, or -1 (none). */
                int current() const { return _current; }

                /** @brief Makes item @p index current (-1: none; not told to `Changed`). */
                void setCurrent(int index);

                /** @brief Returns the index of the first item carrying @p data, or -1. */
                int indexOfData(uintptr_t data) const;

                /** @brief Has @p fn told of the user's choices. */
                void setChanged(Changed fn, void *arg) {
                        _changed = fn;
                        _changedArg = arg;
                        return;
                }

                GuiSize sizeHint() const override;
                void    paint(Painter &painter) override;
                void    mousePressed(const WindowManager::MouseEvent &event) override;
                bool    wheelTurned(int notches) override;
                bool    keyPressed(const KeyEvent &event) override;

        private:
                struct Item {
                                char      text[MaxText + 1] = {};
                                uintptr_t data = 0;
                };

                static void chosen(void *arg, int item);
                void        open();
                void        userSet(int index);

                Item    _items[MaxItems];
                size_t  _count = 0;
                int     _current = -1;
                Menu   *_menu = nullptr; // PSRAM, made at the first opening
                Changed _changed = nullptr;
                void   *_changedArg = nullptr;
};

BMDS_NAMESPACE_END
