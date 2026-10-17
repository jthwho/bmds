/**
 * @file      scrollview.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/scrollarea.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A column of widgets taller than its room, scrolled by whole rows
 *        (`devplan/gui.md` §4u): a settings page.
 *
 * Its children (but its scroll bars) are its rows, top to bottom in the
 * order they were made, each as wide as the viewport and as tall as it asks
 * at that width (`Widget::heightForWidth()`), `setSpacing()` apart inside
 * `setMargins()`. A row is drawn and takes the mouse only inside the
 * viewport (`childClip()`).
 *
 * It scrolls a row at a time: some row is always at the top, never cut, so
 * a widget's rectangle (window coordinates, unsigned) never starts above the
 * viewport. Rows above the top one are kept, whole, out of sight below it.
 * The last row worth scrolling to is the first that shows the rest. The
 * wheel moves a row a notch; the bar's arrows, track and thumb move to the
 * nearest row, at least one row the way they went. A widget inside a row
 * that takes the focus (Tab) is scrolled to.
 *
 * Call `relayout()` after adding, hiding or showing rows, or when one's
 * height changes. Its hint's height is its rows' at its width once it has
 * one (at their hints' before): a layout holding it lays out twice.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class ScrollView : public ScrollArea {
        public:
                /** @brief The most rows laid out (the rest aren't shown). */
                static constexpr size_t MaxRows = 128;

                explicit ScrollView(Widget *parent);

                /** @brief Sets the gap between rows. */
                void setSpacing(uint16_t spacing);

                /** @brief Sets the room kept around the rows (left and right multiples of 4). */
                void setMargins(uint16_t left, uint16_t top, uint16_t right, uint16_t bottom);

                /** @brief Lays the rows out again (after adding, hiding or showing some). */
                void relayout();

                /** @brief Returns the row at the top (its index among the shown rows). */
                size_t topRow() const { return _top; }

                /** @brief Scrolls as little as shows the row holding @p widget (a row or inside one). */
                void reveal(const Widget &widget);

                GuiSize sizeHint() const override;
                bool    childClip(const Widget &child, Rect &clip) const override;
                bool    wheelTurned(int notches) override;
                void    childFocused(Widget &widget) override;

        protected:
                /**
                 * @brief About to ask the rows their heights at @p width (a subclass
                 *        sets what they share: a form's label column).
                 */
                virtual void layingOut(uint16_t width) { (void)width; }

                /** @brief Returns the shown rows into @p out, in order. */
                size_t rows(Widget *(&out)[MaxRows]) const;

                void resized() override;
                void scrolled() override;
                void paintContent(Painter &painter, const Rect &viewport) override;

        private:
                void     measure(uint16_t width); // the rows' heights and stops at @p width
                void     place();                 // the rows where the top row puts them
                void     scrollToRow(size_t row);
                uint32_t stop(size_t row) const { return _stops[row]; }

                uint32_t _tops[MaxRows] = {};    // each row's top in the content
                uint32_t _stops[MaxRows] = {};   // the offset that puts it at the top
                uint16_t _heights[MaxRows] = {}; // and its height
                size_t   _count = 0;
                size_t   _top = 0;  // the row at the top
                size_t   _last = 0; // the last row worth scrolling to
                uint32_t _total = 0;
                uint16_t _spacing = 4;
                uint16_t _margins[4] = {8, 8, 8, 8}; // left, top, right, bottom
};

BMDS_NAMESPACE_END
