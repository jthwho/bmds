/**
 * @file      patchview.h
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
#include <bmds/menu.h>
#include <bmds/widget.h>

#include "patchsource.h"

BMDS_NAMESPACE_BEGIN

class GuiApplication;

/**
 * @brief The patchbay's view (`devplan/audio.md` §24): the ports with a
 *        source on the left ("From"), those with a destination on the right
 *        ("To"), the connections as wires in the band between, each in its
 *        source's colour. Drag from a row to one on the other side, or select
 *        one on each side and press Enter, to connect (or disconnect) them;
 *        Delete disconnects the selected pair; a right click (or the Menu key)
 *        lists a row's connections to end. It follows the graph's changes.
 * @par Thread Safety
 * The application's loop (its lock held), as every widget.
 */
class PatchView final : public Widget {
        public:
                /** @brief Rows a side shows at most (the router's ports). */
                static constexpr size_t MaxRows = 16;

                /** @brief Connections shown at most. */
                static constexpr size_t MaxConnections = 32;

                PatchView(Widget *parent, GuiApplication &app, PatchSource &source);
                ~PatchView() override;

                /** @brief Reads the graph again and redraws. */
                void refresh();

                /** @brief Starts following the graph. @return The watch's error. */
                Error start();

                GuiSize sizeHint() const override;
                void    paint(Painter &painter) override;
                void    mousePressed(const WindowManager::MouseEvent &event) override;
                void    mouseMoved(const WindowManager::MouseEvent &event) override;
                void    mouseReleased(const WindowManager::MouseEvent &event) override;
                bool    keyPressed(const KeyEvent &event) override;
                void    focusIn() override { update(); }
                void    focusOut() override { update(); }

        private:
                enum Side : uint8_t {
                        Left,
                        Right,
                };

                struct Row {
                                uint8_t port = 0;
                                char    name[32] = {};
                                char    tag[16] = {}; // "midi3 usb"
                };

                struct Layout {
                                int16_t  leftX = 0;  // the left column
                                int16_t  rightX = 0; // the right column
                                uint16_t columnWidth = 0;
                                int16_t  top = 0; // the first row's
                                int16_t  bandLeft = 0;
                                int16_t  bandRight = 0;
                };

                static void changed(void *arg);      // any task
                static void refreshLater(void *arg); // the loop
                static void menuChosen(void *arg, int item);

                Layout   layout() const;
                int      rowAt(Side side, int x, int y) const; // -1: none
                int16_t  rowY(size_t index) const;             // a row's middle
                bool     isConnected(uint8_t source, uint8_t destination) const;
                void     toggle(uint8_t source, uint8_t destination);
                void     drawWire(Painter &painter, const Layout &l, size_t from, size_t to, uint32_t color,
                                  uint16_t thick) const;
                void     drawSide(Painter &painter, const Layout &l, Side side) const;
                void     openMenu(Side side, int index, int x, int y);
                void     say(const char *text);
                uint32_t colorOf(size_t sourceIndex) const;

                GuiApplication         &_app;
                PatchSource            &_source;
                Row                     _left[MaxRows];
                Row                     _right[MaxRows];
                size_t                  _leftCount = 0;
                size_t                  _rightCount = 0;
                PatchSource::Connection _connections[MaxConnections];
                size_t                  _connectionCount = 0;
                int                     _selected[2] = {-1, -1};
                Side                    _side = Left;
                // A drag from a row: its side and row, and where the pointer is.
                bool                    _dragging = false;
                Side                    _dragSide = Left;
                int                     _dragRow = -1;
                int16_t                 _dragX = 0;
                int16_t                 _dragY = 0;
                Menu                    _menu;
                Side                    _menuSide = Left;
                int                     _menuRow = -1;
                PatchSource::Connection _menuItems[MaxConnections];
                size_t                  _menuCount = 0;
                char                    _message[96] = {};
                std::atomic<bool>       _posted{false};
                bool                    _watching = false;
};

BMDS_NAMESPACE_END
