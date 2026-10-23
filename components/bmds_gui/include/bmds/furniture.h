/**
 * @file      furniture.h
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

/**
 * @brief A row of flat buttons (an icon, or text) and separators
 *        (`devplan/filebrowser.md` §4e). Tab doesn't stop on its buttons.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class ToolBar : public BoxLayout {
        public:
                explicit ToolBar(Widget *parent);

                /**
                 * @brief Adds a flat button showing @p icon (kept by the caller; null:
                 *        its text instead), clicked to @p fn.
                 * @return The button, or null (no memory).
                 */
                Button *addButton(const Icon *icon, StringView text, Button::Clicked fn, void *arg);

                /** @brief Adds an etched line between groups. @return It, or null (no memory). */
                Widget *addSeparator();

                GuiSize sizeHint() const override;
};

/**
 * @brief A window's status bar: sunken fields in a row
 *        (`devplan/filebrowser.md` §4e).
 *
 * @par Thread Safety
 * As `Widget`.
 */
class StatusBar : public BoxLayout {
        public:
                static constexpr size_t   MaxFields = 6;
                static constexpr uint16_t Height = 24;

                explicit StatusBar(Widget *parent);

                /**
                 * @brief Adds a field @p width wide (0: it stretches to the room left).
                 * @return Its number, or -1 (`MaxFields` already, or no memory).
                 */
                int addField(uint16_t width);

                /** @brief Sets field @p field's text. */
                void setText(size_t field, StringView text);

                GuiSize sizeHint() const override;

        private:
                Label *_fields[MaxFields] = {};
                size_t _count = 0;
};

/**
 * @brief Two widgets side by side (or one above the other) with a handle
 *        between, dragged to share the room (`devplan/filebrowser.md` §4e).
 *
 * Each side keeps at least its least. When the splitter is resized, the
 * first side keeps its size (a folder tree stays as wide). Side by side,
 * the first side's width is a multiple of 4 (the second starts on a column).
 * The pointer over the handle is a double arrow.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class Splitter : public Widget {
        public:
                enum class Orientation : uint8_t {
                        Horizontal, ///< Side by side.
                        Vertical,   ///< One above the other.
                };

                Splitter(Widget *parent, Orientation orientation);

                /** @brief Places its sides @p orientation from now on. */
                void setOrientation(Orientation orientation);

                /** @brief Returns how its sides are placed. */
                Orientation orientation() const { return _orientation; }

                /**
                 * @brief Sets the two sides (its children; null: none), placed again: call
                 *        again after hiding or showing one (a side hidden leaves all the room
                 *        to the other, without a handle).
                 */
                void setWidgets(Widget *first, Widget *second);

                /** @brief Sets the first side's size (kept within the least sizes). */
                void setPosition(uint16_t firstSize);

                /** @brief Returns the first side's size. */
                uint16_t position() const { return _position; }

                /** @brief Sets each side's least size. */
                void setLeast(uint16_t first, uint16_t second);

                void                   paint(Painter &painter) override;
                void                   mousePressed(const WindowManager::MouseEvent &event) override;
                void                   mouseReleased(const WindowManager::MouseEvent &event) override;
                void                   mouseMoved(const WindowManager::MouseEvent &event) override;
                WindowManager::Pointer pointerAt(int x, int y) const override;

        protected:
                void resized() override;

        private:
                Rect     handle() const;
                uint16_t length() const;
                void     place();
                Widget  *alone() const;

                Widget     *_first = nullptr;
                Widget     *_second = nullptr;
                Orientation _orientation;
                uint16_t    _position = 0;
                uint16_t    _wanted = 0; // the size asked for (kept as the room changes)
                uint16_t    _leastFirst = 0;
                uint16_t    _leastSecond = 0;
                int         _grab = -1; // the press on the handle, from the first side's start
};

/**
 * @brief How far along something is: blocks in a sunken field, as Windows
 *        95's (`devplan/filebrowser.md` §4e).
 *
 * @par Thread Safety
 * As `Widget`.
 */
class ProgressBar : public Widget {
        public:
                explicit ProgressBar(Widget *parent);

                /** @brief Sets the maximum (0: nothing shown). */
                void setMaximum(uint64_t maximum);

                /** @brief Sets the value (redrawn only if a block changes). */
                void setValue(uint64_t value);

                /** @brief Returns the value. */
                uint64_t value() const { return _value; }

                GuiSize sizeHint() const override;
                void    paint(Painter &painter) override;

        private:
                uint16_t inner() const; // the blocks' room

                uint64_t _maximum = 100;
                uint64_t _value = 0;
};

BMDS_NAMESPACE_END
