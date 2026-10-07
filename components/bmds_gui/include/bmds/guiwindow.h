/**
 * @file      guiwindow.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/guiapplication.h>
#include <bmds/region.h>
#include <bmds/widget.h>
#include <bmds/windowmanager.h>

BMDS_NAMESPACE_BEGIN


/**
 * @brief A window on the window manager: the root of its widgets
 *        (`devplan/gui.md` §4b).
 *
 * Its geometry is its client area (from 0, 0). The central widget, if set,
 * fills it; else it is filled in the face's color. The mouse goes to the
 * deepest widget under it, and after a press to the pressed widget until the
 * release; keys to the focus widget. `update()`s are gathered and sent to
 * the window manager after each round of events.
 *
 * @par Thread Safety
 * The application's lock (handlers and drawing hold it).
 */
class GuiWindow : public Widget {
        public:
                /** @brief How the window is opened. */
                struct Config {
                                Rect                   rect; ///< Outer, frame included.
                                const char            *title = "";
                                uint8_t                decorations = WindowFrame::Standard;
                                WindowManager::Level   level = WindowManager::Level::Normal;
                                uint16_t               reserve = 0; ///< A panel's strip.
                                bool                   focusable = true;
                                const char            *kind = nullptr;
                                WindowManager::Pointer pointer = WindowManager::Pointer::Arrow;
                                bool reference = false; ///< It draws a reference picture (`drawReference()`).
                };

                /** @brief Told the window was closed by the window manager (its close button). */
                using Closed = void (*)(void *arg, GuiWindow &window);

                GuiWindow(GuiApplication &app, const Config &config);
                ~GuiWindow() override;

                /** @brief Returns its application. */
                GuiApplication &application() const { return _app; }

                /**
                 * @brief Opens it on the screen (at the config's rectangle).
                 * @return `Ok` (already open too), or the window manager's error.
                 */
                Error show();

                /** @brief Opens it at @p rect (outer) instead. */
                Error show(const Rect &rect);

                /** @brief Closes it (it may be shown again). */
                void close();

                /** @brief Returns true while open. */
                bool isShown() const { return _id.load(std::memory_order_acquire) != 0; }

                /** @brief Returns its window manager's number (0: not open). */
                uint8_t id() const { return _id.load(std::memory_order_acquire); }

                /** @brief Returns its client area on the screen (empty: not open). */
                Rect screenRect() const { return _screen; }

                /** @brief Sets the widget that fills it (one of its children). */
                void setCentral(Widget *widget);

                /**
                 * @brief Sets its icon (null: none), kept by the caller: the title
                 *        bar's, the panel's (16 × 16).
                 */
                void setIcon(const Icon *icon);

                /** @brief Sets its large icon (null: none; 32 × 32), kept by the caller: Alt+Tab's. */
                void setLargeIcon(const Icon *icon);

                /** @brief Sets the widget keys go to (null: none). */
                void setFocusWidget(Widget *widget) { _focus = widget; }

                /** @brief Has @p fn told when the window manager closes it. */
                void setClosed(Closed fn, void *arg) {
                        _closed = fn;
                        _closedArg = arg;
                        return;
                }

                /** @brief Asks for @p rect (window coordinates) to be redrawn. */
                void invalidate(const Rect &rect);

                /**
                 * @brief Moves the gathered redraws into @p out (the application's,
                 *        after events, to give them to the window manager once its
                 *        lock is released).
                 * @return The window's number, or 0 (not shown).
                 */
                uint8_t takeDirty(Region &out);

                /** @brief Forgets the widget being destroyed (`Widget::destroy()`). */
                void forget(Widget *widget);

                /** @brief An event for it (`GuiApplication`). */
                void handle(const GuiApplication::Event &event);

                void paint(Painter &painter) override;

        protected:
                /** @brief The window manager closed it (its close button, a popup's dismissal). */
                virtual void closedByManager();

                /**
                 * @brief Draws what its client area should show into @p pixels (B, G,
                 *        R; `win check`, a test aid), when the config says it can.
                 *        On the caller's task, the window manager locked.
                 */
                virtual void drawReference(uint8_t *pixels, size_t stride, uint16_t width, uint16_t height,
                                           const WindowPalette &palette) {
                        (void)pixels;
                        (void)stride;
                        (void)width;
                        (void)height;
                        (void)palette;
                        return;
                }

                /** @brief A key no widget used. @return True if used. */
                virtual bool keyUnused(const KeyEvent &event) {
                        (void)event;
                        return false;
                }

                void resized() override;

        private:
                static void redraw(void *arg, Painter &painter);
                static void closedEntry(void *arg);
                static void keyEntry(void *arg, const KeyEvent &event);
                static void resizedEntry(void *arg, uint16_t width, uint16_t height);
                static void wheelEntry(void *arg, int notches);
                static void mouseEntry(void *arg, const WindowManager::MouseEvent &event);
                static void referenceEntry(void *arg, uint8_t *pixels, size_t stride, uint16_t width, uint16_t height,
                                           const WindowPalette &palette);
                void        queue(GuiApplication::Event event);
                void        paintTree(Widget &widget, Painter &painter);
                void        mouse(const WindowManager::MouseEvent &event);

                GuiApplication      &_app;
                Config               _config;
                char                 _title[WindowManager::MaxTitle + 1] = {};
                char                 _kind[WindowManager::MaxKind + 1] = {};
                std::atomic<uint8_t> _id{0};
                uint8_t              _slot = 0;
                uint8_t              _generation = 0;
                Rect                 _screen;
                Widget              *_central = nullptr;
                const Icon          *_icon = nullptr;
                const Icon          *_largeIcon = nullptr;
                Widget              *_focus = nullptr;
                Widget              *_grab = nullptr;  // pressed: has the mouse until the release
                Widget              *_hover = nullptr; // under the pointer
                Region               _dirty;
                Closed               _closed = nullptr;
                void                *_closedArg = nullptr;
};

BMDS_NAMESPACE_END
