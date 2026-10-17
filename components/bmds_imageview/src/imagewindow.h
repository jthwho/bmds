/**
 * @file      imagewindow.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/filedialog.h>
#include <bmds/folderwatches.h>
#include <bmds/furniture.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/images.h>
#include <bmds/list.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/string.h>
#include <bmds/thread.h>

#include "viewlayout.h"

BMDS_NAMESPACE_BEGIN

class ImageWindow;

/**
 * @brief The picture (`devplan/images.md` §4b): what the worker made for the
 *        view, drawn by the 2D-DMA on a dark ground; dragged to pan, the
 *        wheel to step or pan, a double click to maximize.
 *
 * @par Thread Safety
 * The application's lock.
 */
class PictureView final : public Widget {
        public:
                PictureView(Widget *parent, ImageWindow &window);

                void    paint(Painter &painter) override;
                void    mousePressed(const WindowManager::MouseEvent &event) override;
                void    mouseMoved(const WindowManager::MouseEvent &event) override;
                void    mouseReleased(const WindowManager::MouseEvent &event) override;
                bool    wheelTurned(int notches) override;
                GuiSize sizeHint() const override;
                void    dragMoved(GuiDropEvent &event) override; // files dropped open (clipboard.md §5e)
                bool    dropped(GuiDropEvent &event) override;

        protected:
                void resized() override;

        private:
                static void dropRead(void *arg, Error err, StringView type, StringView data);

                ImageWindow &_window;
                uint32_t     _dropDrag = 0; // a drop being read
                int          _dragX = 0; // where a drag started, and its pan then
                int          _dragY = 0;
                int32_t      _dragPanX = 0;
                int32_t      _dragPanY = 0;
                bool         _dragging = false;
                uint32_t     _lastPressMs = 0; // a double click's first
};

/**
 * @brief The image viewer's window (`devplan/images.md` §4): a toolbar, the
 *        picture, a status bar; the folder's pictures in the file browser's
 *        order, stepped through by key, wheel and buttons. Decoding and
 *        scaling run on a thread of its own, the next picture decoded ahead.
 *
 * @par Thread Safety
 * The application's lock; the worker posts its results to the loop.
 */
class ImageWindow final : public GuiWindow {
        public:
                ImageWindow(GuiApplication &app, const Config &config);
                ~ImageWindow() override;

                /** @brief Makes its widgets and starts the worker. */
                Error build();

                /** @brief Opens @p paths: one, with its folder's other pictures; several, just them. */
                void open(Span<const StringView> paths);

                // ---- The view's
                void    paintPicture(Painter &painter, const Rect &area);
                void    viewResized();
                void    panBy(int dx, int dy, bool settled);
                void    panTo(int32_t x, int32_t y, bool settled);
                void    toggleMaximized();
                void    step(int by);       // through the list
                void    wheel(int notches); // stepped (fitted) or panned
                int32_t panX() const { return _in.panX; }
                int32_t panY() const { return _in.panY; }
                bool    pans() const { return _layout.pans(_in); }

        protected:
                bool keyUnused(const KeyEvent &event) override;

        private:
                static constexpr uint32_t LoadingAfterMs = 150;
                static constexpr size_t   StackBytes = 8192;

                // What the worker is asked to make: the layout's part of a picture (its size the worker's
                // to know), then the next one decoded ahead.
                struct Request {
                                uint32_t          generation = 0;
                                int32_t           index = -1;
                                String            path{MemSpace::Psram};
                                uint8_t           turns = 0; // the user's quarter turns, right
                                ViewLayout::Input input;
                                int32_t           ahead = -1;
                                String            aheadPath{MemSpace::Psram};
                };

                // What the worker made.
                struct Made {
                                uint32_t        generation = 0;
                                int32_t         index = -1;
                                Images::Picture shown;
                                ViewLayout      layout;
                                uint32_t        sourceWidth = 0; // the picture as decoded (turned)
                                uint32_t        sourceHeight = 0;
                                uint32_t        fileWidth = 0; // the file's (turned)
                                uint32_t        fileHeight = 0;
                                Error           error;
                };

                // A picture the worker decoded.
                struct Decoded {
                                Images::Picture picture;
                                int32_t         index = -1;
                                uint8_t         turns = 0;
                                bool            reduced = false; // smaller than the file (fitted to a box)
                                uint32_t        fileWidth = 0;
                                uint32_t        fileHeight = 0;
                };

                enum Tool : int {
                        ToolOpen,
                        ToolPrevious,
                        ToolNext,
                        ToolFit,
                        ToolActual,
                        ToolZoomOut,
                        ToolZoomIn,
                        ToolTurn,
                        ToolMax
                };

                static void toolClicked(void *arg, Button &button);
                static void workerMain(void *arg);
                static void resultDue(void *arg);
                static void loadingDue(void *arg);
                static void settleDue(void *arg);
                static void folderChanged(void *arg); // any task
                static void folderDue(void *arg);
                static void folderTaken(void *arg, StringView path, uint8_t changes);
                static void opened(void *arg, FileDialog &dialog, int result);
                void        work(); // the worker
                Error       decode(StringView path, int32_t index, uint8_t turns, const ViewLayout::Input &in,
                                   Decoded &out);              // the worker
                void        render(const Request &q, Made &r); // the worker
                void        request();
                void        relayout();
                void        listFolder(StringView folder, StringView select);
                void        showStatus();
                void        setZoom(bool fit, uint32_t zoom);
                String      pathOf(int32_t index) const;

                PictureView *_view = nullptr;
                ToolBar     *_tools = nullptr;
                StatusBar   *_status = nullptr;
                Button      *_toolButtons[ToolMax] = {};
                FileDialog   _openDialog;

                // The list: the folder (or the files opened) and the names in it, in order.
                String            _folder{MemSpace::Psram};
                List<String>      _names{MemSpace::Psram};
                int32_t           _index = -1;
                bool              _single = true; // the folder's pictures (else just those opened)
                FolderWatches     _watches{folderChanged, this};
                std::atomic<bool> _folderPosted{false};

                // The view: what is asked, what shows.
                ViewLayout::Input _in;
                ViewLayout        _layout;
                uint8_t           _turns = 0;
                Images::Picture   _shown;       // the layout's part, made by the worker
                ViewLayout        _shownLayout; // what it was made for (its pan: a drag moves the view meanwhile)
                int32_t           _shownIndex = -1;
                uint32_t          _fileWidth = 0;
                uint32_t          _fileHeight = 0;
                Error             _error;
                bool              _loading = false;
                int               _loadingTimer = -1;
                int               _settleTimer = -1;
                bool              _maximized = false;
                char              _message[160] = {};

                // The worker: one request at a time, the latest; one result.
                Thread            _thread;
                Mutex             _lock; // the request and the result
                Semaphore         _wake{1, 0};
                Request           _request;
                Made              _result;
                bool              _quit = false;
                std::atomic<bool> _posted{false};
                uint32_t          _generation = 0;
                // The worker's own: the picture decoded, and the next.
                Decoded _source;
                Decoded _ahead;
};

BMDS_NAMESPACE_END
