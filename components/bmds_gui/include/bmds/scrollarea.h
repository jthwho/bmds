/**
 * @file      scrollarea.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/scrollmodel.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A scroll bar (`devplan/filebrowser.md` §4e): arrows at its ends,
 *        a track, and a thumb sized to the page (`ScrollModel`).
 *
 * A press on an arrow moves a line, on the track a page, and either repeats
 * while held (after 400 ms, every 50 ms; the track's until the thumb reaches
 * the pointer). The thumb is dragged. Changes made by the user are told to
 * `Changed`; `setValue()` isn't. Tab doesn't stop on it.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class ScrollBar : public Widget {
        public:
                /** @brief Told the value the user moved it to (on the loop's thread). */
                using Changed = void (*)(void *arg, ScrollBar &bar, uint32_t value);

                /** @brief Which way it runs. */
                enum class Orientation : uint8_t {
                        Vertical,
                        Horizontal,
                };

                /** @brief Its thickness, and an arrow's length. */
                static constexpr uint16_t Breadth = 16;

                ScrollBar(Widget *parent, Orientation orientation);
                ~ScrollBar() override;

                /** @brief Returns which way it runs. */
                Orientation orientation() const { return _orientation; }

                /** @brief Returns what it shows. */
                const ScrollModel &model() const { return _model; }

                /** @brief Sets the range and the page that shows of it (redrawn if that changes the bar). */
                void setRange(uint32_t total, uint32_t page);

                /** @brief Sets a line's length (an arrow's step). */
                void setLineStep(uint32_t line) { _model.setLineStep(line); }

                /** @brief Returns the value. */
                uint32_t value() const { return _model.value(); }

                /** @brief Sets the value (kept in range; not told to `Changed`). */
                void setValue(int64_t value);

                /** @brief Has @p fn told of the user's changes. */
                void setChanged(Changed fn, void *arg) {
                        _changed = fn;
                        _changedArg = arg;
                        return;
                }

                GuiSize sizeHint() const override;
                void    paint(Painter &painter) override;
                void    mousePressed(const WindowManager::MouseEvent &event) override;
                void    mouseReleased(const WindowManager::MouseEvent &event) override;
                void    mouseMoved(const WindowManager::MouseEvent &event) override;

        private:
                using Part = ScrollModel::Part;

                uint16_t              length() const; // along it
                int                   along(int x, int y) const;
                ScrollModel::Geometry parts() const { return _model.geometry(length(), Breadth); }
                Rect                  partRect(uint16_t start, uint16_t size) const; // along it
                void                  act();                                         // the pressed part once, told
                void                  stopRepeat();
                static void           tick(void *arg);

                ScrollModel _model;
                Changed     _changed = nullptr;
                void       *_changedArg = nullptr;
                Orientation _orientation;
                Part        _pressed = Part::None;
                int         _at = 0;            // the pointer along it, while pressed
                int         _grab = 0;          // the thumb's press, from its start
                int         _timer = -1;        // the repeat's
                bool        _repeating = false; // past the first delay
};

/**
 * @brief A viewport on content larger than it, with scroll bars as needed
 *        (`devplan/filebrowser.md` §4e; as Qt's `QAbstractScrollArea`).
 *
 * The content is drawn by `paintContent()` through a painter clipped to the
 * viewport, its point (cx, cy) at (`viewport().x + cx - scrollX()`,
 * `viewport().y + cy - scrollY()`) in the window: a subclass draws only what
 * shows. The horizontal offset moves in 4-pixel steps (text starts on a
 * 4-pixel column). The wheel scrolls three lines a notch; the bars, their
 * arrows and track as `ScrollBar`. A scroll redraws the viewport.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class ScrollArea : public Widget {
        public:
                explicit ScrollArea(Widget *parent);

                /** @brief Sets the content's size (the bars and the offset follow). */
                void setContentSize(uint32_t width, uint32_t height);

                /** @brief Returns the content's width. */
                uint32_t contentWidth() const { return _contentWidth; }

                /** @brief Returns the content's height. */
                uint32_t contentHeight() const { return _contentHeight; }

                /** @brief Sets a line's length across (rounded up to 4) and down (an arrow's or a wheel notch's third). */
                void setLineSteps(uint16_t x, uint16_t y);

                /** @brief Returns how far the content is scrolled left. */
                uint32_t scrollX() const { return _scrollX; }

                /** @brief Returns how far the content is scrolled up. */
                uint32_t scrollY() const { return _scrollY; }

                /** @brief Scrolls to (@p x, @p y), kept in range (x in 4-pixel steps). */
                void scrollTo(int64_t x, int64_t y);

                /** @brief Scrolls as little as shows content rectangle (@p x, @p y, @p width, @p height). */
                void ensureVisible(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

                /** @brief Returns the viewport (window coordinates): it less the bars. */
                Rect viewport() const;

                /** @brief Returns the window x of content x @p contentX. */
                int windowX(uint32_t contentX) const { return int(viewport().x) + int(contentX) - int(_scrollX); }

                /** @brief Returns the window y of content y @p contentY. */
                int windowY(uint32_t contentY) const { return int(viewport().y) + int(contentY) - int(_scrollY); }

                bool wheelTurned(int notches) override;
                void paint(Painter &painter) override;

        protected:
                /** @brief Draws the content (the viewport's damage) through @p painter, clipped to @p viewport. */
                virtual void paintContent(Painter &painter, const Rect &viewport);

                /** @brief The offset changed (the viewport is redrawn already). */
                virtual void scrolled() {}

                void resized() override;

        private:
                void        layoutBars();
                void        follow(); // the offset from the bars
                static void barChanged(void *arg, ScrollBar &bar, uint32_t value);

                ScrollBar *_vertical = nullptr;
                ScrollBar *_horizontal = nullptr;
                uint32_t   _contentWidth = 0;
                uint32_t   _contentHeight = 0;
                uint32_t   _scrollX = 0;
                uint32_t   _scrollY = 0;
                uint16_t   _lineX = 16;
                uint16_t   _lineY = 16;
};

BMDS_NAMESPACE_END
