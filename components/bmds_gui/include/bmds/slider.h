/**
 * @file      slider.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/slidermodel.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A handle moved along a groove to choose a value in a range
 *        (`devplan/gui.md` §4s). A vertical one has its maximum at the top.
 *
 * Drag the handle, or press the groove to bring the handle there and drag
 * it; the wheel moves it a step a notch; the arrows a step, Page Up and Page
 * Down a page, Home and End to the ends. It takes the focus.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class Slider : public Widget {
        public:
                /** @brief Told each value the user moves it to (on the loop's thread). */
                using Changed = void (*)(void *arg, Slider &slider, int32_t value);

                /** @brief Told the user let the handle go at @p value, after a drag (on the loop's thread). */
                using Released = void (*)(void *arg, Slider &slider, int32_t value);

                /** @brief Which way it runs. */
                enum class Orientation : uint8_t {
                        Vertical,
                        Horizontal,
                };

                /** @brief Its thickness. */
                static constexpr uint16_t Breadth = 22;

                /** @brief The handle's length along it. */
                static constexpr uint16_t HandleLength = 11;

                Slider(Widget *parent, Orientation orientation);

                /** @brief Makes it run @p orientation from now on. */
                void setOrientation(Orientation orientation);

                /** @brief Returns which way it runs. */
                Orientation orientation() const { return _orientation; }

                /** @brief Returns what it shows. */
                const SliderModel &model() const { return _model; }

                /** @brief Sets the range (the value kept in it; redrawn). */
                void setRange(int32_t minimum, int32_t maximum);

                /** @brief Sets a step and a page. */
                void setSteps(int32_t step, int32_t page) { _model.setSteps(step, page); }

                /** @brief Returns the value. */
                int32_t value() const { return _model.value(); }

                /** @brief Sets the value (kept in range; not told to `Changed`). */
                void setValue(int64_t value);

                /** @brief Has @p fn told of the user's changes. */
                void setChanged(Changed fn, void *arg) {
                        _changed = fn;
                        _changedArg = arg;
                        return;
                }

                /**
                 * @brief Has @p fn told when a drag ends (a seek bar seeks then, its
                 *        `Changed` only showing the place). Keys and the wheel are
                 *        told to `Changed` alone.
                 */
                void setReleased(Released fn, void *arg) {
                        _released = fn;
                        _releasedArg = arg;
                        return;
                }

                /** @brief Returns true while the user drags the handle (`setValue()` would fight it). */
                bool isDragging() const { return _dragging; }

                GuiSize sizeHint() const override;
                void    paint(Painter &painter) override;
                void    mousePressed(const WindowManager::MouseEvent &event) override;
                void    mouseReleased(const WindowManager::MouseEvent &event) override;
                void    mouseMoved(const WindowManager::MouseEvent &event) override;
                bool    wheelTurned(int notches) override;
                bool    keyPressed(const KeyEvent &event) override;

        private:
                uint16_t length() const;                                  // along it
                int      along(const WindowManager::MouseEvent &e) const; // the pointer, from its start
                Rect     handleRect() const;
                void     userSet(int64_t value); // set, redrawn, told

                SliderModel _model;
                Orientation _orientation;
                Changed     _changed = nullptr;
                void       *_changedArg = nullptr;
                Released    _released = nullptr;
                void       *_releasedArg = nullptr;
                bool        _dragging = false;
                int         _grab = 0; // where on the handle it was taken
};

BMDS_NAMESPACE_END
