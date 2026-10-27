/**
 * @file      imagewindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The image viewer's window (devplan/images.md §4). The loop asks; a worker thread decodes
 * (Images: the codec, TJpgDec, PNG, BMP) and scales the part that shows (ImageScaler), and posts
 * the result. The worker keeps the picture decoded and the next one; the loop keeps what shows.
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <bmds/bitmapfont.h>
#include <bmds/foldermodel.h>
#include <bmds/iconid.h>
#include <bmds/icontheme.h>
#include <bmds/imagescaler.h>
#include <bmds/inputevent.h>
#include <bmds/painter.h>
#include <bmds/system.h>

#include "imagewindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t Ground = 0x2a2a2a; // around the picture
        constexpr uint32_t TextColor = 0xd8d8d8;
        constexpr int      PanStep = 64;   // an arrow key's pan
        constexpr uint32_t SettleMs = 120; // after a drag or a resize, before the part is made again
        constexpr char     Title[] = "Image Viewer";
        constexpr char     Patterns[] = "*.png;*.jpg;*.jpeg;*.bmp";

        uint32_t nowMs() {
                return uint32_t(System::uptimeUs() / 1000);
        }

        // EXIF's code for the user's quarter turns to the right.
        uint8_t orientationOf(uint8_t turns) {
                static constexpr uint8_t Codes[4] = {1, 6, 3, 8};
                return Codes[turns & 3];
        }

        StringView nameOf(StringView path) {
                const size_t slash = path.lastIndexOf('/');
                return slash == StringView::NPos ? path : path.mid(slash + 1);
        }

        bool isPicture(StringView name) {
                const ImageCodec c = Images::codecForName(name);
                return c == ImageCodec::Png || c == ImageCodec::Jpeg || c == ImageCodec::Bmp;
        }

} // namespace

// ---- PictureView

PictureView::PictureView(Widget *parent, ImageWindow &window) : Widget(parent), _window(window) {
        setStretch(1);
        setAcceptsDrops(true);
}

// Files dropped on the picture open (devplan/clipboard.md §5e): one with its folder, several alone.
void PictureView::dragMoved(GuiDropEvent &event) {
        event.accept(event.hasType("text/uri-list") && DropActions::allows(event.actions(), DropAction::Copy)
                             ? DropAction::Copy
                             : DropAction::None);
        return;
}

bool PictureView::dropped(GuiDropEvent &event) {
        if (!event.hasType("text/uri-list") || !DropActions::allows(event.actions(), DropAction::Copy)) return false;
        if (event.read(ClipboardType::Text, dropRead, this).isError()) return false; // the paths, a line each
        _dropDrag = event.drag();
        return true;
}

void PictureView::dropRead(void *arg, Error err, StringView type, StringView data) {
        (void)type;
        auto          *self = static_cast<PictureView *>(arg);
        const uint32_t drag = self->_dropDrag;
        self->_dropDrag = 0;
        StringView paths[16];
        size_t     n = 0;
        for (StringView rest = err.isOk() ? data : StringView(); !rest.isEmpty() && n < 16;) {
                const size_t     end = rest.indexOf('\n');
                const StringView p = end == StringView::NPos ? rest : rest.left(end);
                rest = end == StringView::NPos ? StringView() : rest.mid(end + 1);
                if (!p.isEmpty()) paths[n++] = p;
        }
        GuiWindow *w = self->window();
        if (w != nullptr)
                (void)w->application().windowManager().finishDrop(drag, n > 0 ? DropAction::Copy : DropAction::None);
        if (n > 0) self->_window.open(Span<const StringView>(paths, n));
        return;
}

GuiSize PictureView::sizeHint() const {
        return GuiSize{64, 64};
}

void PictureView::resized() {
        _window.viewResized();
        return;
}

void PictureView::paint(Painter &painter) {
        _window.paintPicture(painter, geometry());
        return;
}

void PictureView::mousePressed(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left) return;
        const uint32_t now = nowMs();
        if (now - _lastPressMs < WindowManager::DoubleClickMs) {
                _lastPressMs = 0;
                _window.toggleMaximized();
                return;
        }
        _lastPressMs = now;
        _dragging = _window.pans();
        _dragX = event.x;
        _dragY = event.y;
        _dragPanX = _window.panX();
        _dragPanY = _window.panY();
        return;
}

void PictureView::mouseMoved(const WindowManager::MouseEvent &event) {
        if (!_dragging || (event.buttons & PointerEvent::Left) == 0) return;
        _window.panTo(_dragPanX - (event.x - _dragX), _dragPanY - (event.y - _dragY), false);
        return;
}

void PictureView::mouseReleased(const WindowManager::MouseEvent &event) {
        if (!_dragging) return;
        _dragging = false;
        _window.panTo(_dragPanX - (event.x - _dragX), _dragPanY - (event.y - _dragY), true);
        return;
}

bool PictureView::wheelTurned(int notches) {
        _window.wheel(notches);
        return true;
}

// ---- ImageWindow

ImageWindow::ImageWindow(GuiApplication &app, const Config &config)
    : GuiWindow(app, config), _openDialog(app, FileDialog::Mode::Open, "Open Picture") {}

ImageWindow::~ImageWindow() {
        {
                Mutex::Locker lock(_lock, WaitForever);
                _quit = true;
        }
        _wake.release();
        _thread.join();
        application().cancelPosts(this);
        if (_loadingTimer >= 0) application().stopTimer(_loadingTimer);
        if (_settleTimer >= 0) application().stopTimer(_settleTimer);
}

Error ImageWindow::build() {
        auto *column = Widget::create<BoxLayout>(this, BoxLayout::Direction::Vertical);
        if (column == nullptr) return Error::NoMem;
        setCentral(column);
        _tools = Widget::create<ToolBar>(column);
        _view = Widget::create<PictureView>(column, *this);
        _status = Widget::create<StatusBar>(column);
        if (_tools == nullptr || _view == nullptr || _status == nullptr) return Error::NoMem;
        IconTheme &theme = application().icons();
        struct ToolSpec {
                        Tool        tool;
                        IconId      icon;
                        const char *text;
                        bool        separator;
                        bool        hasIcon = true;
        };
        constexpr ToolSpec Specs[] = {
                {ToolOpen, IconId::DocumentOpen, "Open", true},
                {ToolPrevious, IconId::GoPrevious, "Previous", false},
                {ToolNext, IconId::GoNext, "Next", true},
                {ToolFit, IconId::ImageViewer, "Fit", false, false},
                {ToolActual, IconId::ImageViewer, "1:1", false, false},
                {ToolZoomOut, IconId::ZoomOut, "Zoom Out", false},
                {ToolZoomIn, IconId::ZoomIn, "Zoom In", true},
                {ToolTurn, IconId::ObjectRotateRight, "Turn", false},
        };
        for (const ToolSpec &t : Specs) {
                const Icon *icon = t.hasIcon ? theme.find(t.icon, 16) : nullptr;
                _toolButtons[t.tool] = _tools->addButton(icon, t.text, toolClicked, this);
                if (t.separator) (void)_tools->addSeparator();
        }
        (void)_status->addField(0);   // the name
        (void)_status->addField(120); // its size
        (void)_status->addField(64);  // the zoom
        (void)_status->addField(96);  // its place in the list
        (void)_openDialog.addFilter("Pictures", Patterns);
        (void)_openDialog.addFilter("All files", "");
        _openDialog.setFinished(opened, this);
        setIcon(theme.find(IconId::ImageViewer, 16));
        setLargeIcon(theme.find(IconId::ImageViewer, 32));
        Thread::Config tc;
        tc.name = "imageview";
        tc.stackBytes = StackBytes;
        return _thread.start(tc, workerMain, this);
}

// ---- The list

void ImageWindow::open(Span<const StringView> paths) {
        _names.clear();
        _index = -1;
        if (!_folder.isEmpty()) _watches.remove(_folder.view());
        _folder.clear();
        if (paths.size() == 1) {
                const StringView p = paths[0];
                const size_t     slash = p.lastIndexOf('/');
                _single = true;
                listFolder(slash == 0 ? StringView("/") : p.left(slash), p.mid(slash + 1));
                (void)_watches.add(_folder.view());
        } else {
                // Several: just them, in the order given (full paths, no folder).
                _single = false;
                for (const StringView p : paths) {
                        String s(MemSpace::Psram);
                        if (s.append(p).isOk()) (void)_names.append(std::move(s));
                }
                _index = _names.isEmpty() ? -1 : 0;
        }
        _turns = 0;
        setZoom(true, ViewLayout::Actual);
        request();
        return;
}

void ImageWindow::listFolder(StringView folder, StringView select) {
        String keep(MemSpace::Psram);
        (void)keep.append(select);
        (void)_folder.assign(folder);
        _names.clear();
        _index = -1;
        auto [dir, err] = Vfs::openDirectory(folder, Vfs::Context{});
        if (err.isOk()) {
                for (;;) {
                        auto [e, nerr] = dir.next();
                        if (nerr.isError()) break;
                        if (e.type != FileType::File || !isPicture(e.name.view())) continue;
                        (void)_names.append(std::move(e.name));
                }
        }
        if (_names.size() > 1)
                std::sort(&_names[0], &_names[0] + _names.size(), [](const String &a, const String &b) {
                        return FolderModel::compareNames(a.view(), b.view()) < 0;
                });
        for (size_t i = 0; i < _names.size(); ++i)
                if (_names[i].view() == keep.view()) _index = int32_t(i);
        // The file asked for, even if its name isn't a picture's (opened by its type).
        if (_index < 0 && !keep.isEmpty()) {
                (void)_names.insert(0, std::move(keep));
                _index = 0;
        }
        return;
}

String ImageWindow::pathOf(int32_t index) const {
        String s(MemSpace::Psram);
        if (index < 0 || size_t(index) >= _names.size()) return s;
        if (_single) {
                (void)s.append(_folder.view());
                if (!_folder.view().endsWith("/")) (void)s.append('/');
        }
        (void)s.append(_names[size_t(index)].view());
        return s;
}

void ImageWindow::step(int by) {
        if (_names.isEmpty()) return;
        int32_t i = _index + by;
        if (i < 0) i = 0;
        if (i >= int32_t(_names.size())) i = int32_t(_names.size()) - 1;
        if (i == _index) return;
        _index = i;
        _turns = 0;
        setZoom(true, ViewLayout::Actual);
        request();
        return;
}

void ImageWindow::folderChanged(void *arg) {
        auto *self = static_cast<ImageWindow *>(arg);
        if (self->_folderPosted.exchange(true)) return;
        if (self->application().post(folderDue, self).isError()) self->_folderPosted = false;
        return;
}

void ImageWindow::folderDue(void *arg) {
        auto *self = static_cast<ImageWindow *>(arg);
        self->_folderPosted = false;
        self->_watches.take(folderTaken, self);
        return;
}

void ImageWindow::folderTaken(void *arg, StringView path, uint8_t) {
        auto *self = static_cast<ImageWindow *>(arg);
        if (!self->_single || path != self->_folder.view()) return;
        // Read again, the picture shown kept (or the one now in its place).
        String current(MemSpace::Psram);
        if (self->_index >= 0) (void)current.append(self->_names[size_t(self->_index)].view());
        const int32_t was = self->_index;
        String        folder(MemSpace::Psram);
        (void)folder.append(self->_folder.view());
        self->listFolder(folder.view(), current.view());
        if (self->_index < 0 && !self->_names.isEmpty())
                self->_index =
                        was < int32_t(self->_names.size()) ? (was < 0 ? 0 : was) : int32_t(self->_names.size()) - 1;
        self->request();
        return;
}

// ---- The view

// A new zoom keeps the point at the view's centre there (from a fit: the picture's centre).
void ImageWindow::setZoom(bool fit, uint32_t zoom) {
        const ViewLayout before = _layout;
        _in.fit = fit;
        _in.zoom = zoom;
        if (fit || before.width == 0 || before.height == 0) {
                _in.panX = 0;
                _in.panY = 0;
                return;
        }
        const ViewLayout after = ViewLayout::make(_in);
        const int64_t    cx = int64_t(before.panX) + int64_t(_in.viewWidth) / 2 - int64_t(before.x);
        const int64_t    cy = int64_t(before.panY) + int64_t(_in.viewHeight) / 2 - int64_t(before.y);
        _in.panX = int32_t(cx * after.width / before.width - int64_t(_in.viewWidth) / 2);
        _in.panY = int32_t(cy * after.height / before.height - int64_t(_in.viewHeight) / 2);
        return;
}

void ImageWindow::relayout() {
        if (_view != nullptr) {
                const Rect r = _view->geometry();
                _in.viewWidth = r.width;
                _in.viewHeight = r.height;
        }
        _layout = ViewLayout::make(_in);
        _in.panX = _layout.panX;
        _in.panY = _layout.panY;
        return;
}

void ImageWindow::viewResized() {
        relayout();
        // Made again once the size settles; meanwhile the old part shows where it fits.
        if (_settleTimer >= 0) application().stopTimer(_settleTimer);
        const Result<int> t = application().startTimer(SettleMs, false, settleDue, this);
        _settleTimer = t.isOk() ? t.value() : -1;
        if (_settleTimer < 0) request();
        return;
}

void ImageWindow::settleDue(void *arg) {
        auto *self = static_cast<ImageWindow *>(arg);
        self->_settleTimer = -1;
        self->request();
        return;
}

void ImageWindow::panTo(int32_t x, int32_t y, bool settled) {
        _in.panX = x;
        _in.panY = y;
        relayout();
        if (_view != nullptr) _view->update();
        if (settled) request();
        return;
}

void ImageWindow::panBy(int dx, int dy, bool settled) {
        panTo(_in.panX + dx, _in.panY + dy, settled);
        return;
}

void ImageWindow::wheel(int notches) {
        if (_in.fit || !pans()) {
                step(notches > 0 ? -1 : 1);
                return;
        }
        panBy(0, -notches * PanStep, true);
        return;
}

void ImageWindow::toggleMaximized() {
        WindowManager &wm = application().windowManager();
        const Error    err = _maximized ? wm.restore(id()) : wm.maximize(id());
        if (err.isOk()) _maximized = !_maximized;
        return;
}

void ImageWindow::paintPicture(Painter &painter, const Rect &area) {
        // The part made by the worker, where it goes now (a drag moves the view before it is made again).
        Rect pic;
        if (_shown.isValid() && _shownIndex == _index) {
                // At the same zoom it moves with the pan (a drag before it is made again); else as made.
                const ViewLayout &s = _shownLayout;
                const bool        same = s.width == _layout.width && s.height == _layout.height;
                const int32_t x = int32_t(area.x) + (same ? int32_t(_layout.x) + s.panX - _layout.panX : int32_t(s.x));
                const int32_t y = int32_t(area.y) + (same ? int32_t(_layout.y) + s.panY - _layout.panY : int32_t(s.y));
                // Cut to the view (the painter cuts only to the window).
                int32_t left = x < area.x ? area.x : x, top = y < area.y ? area.y : y;
                int32_t right = x + int32_t(_shown.image().width), bottom = y + int32_t(_shown.image().height);
                if (right > area.right()) right = area.right();
                if (bottom > area.bottom()) bottom = area.bottom();
                left = (left + 3) & ~3;
                right &= ~3;
                if (right > left && bottom > top) {
                        const ImageBuffer &b = _shown.image();
                        const uint8_t     *from = b.row(uint32_t(top - y)) + size_t(left - x) * 3;
                        (void)painter.pixels(left, top, from, b.stride, uint16_t(right - left), uint16_t(bottom - top));
                        pic = Rect{uint16_t(left), uint16_t(top), uint16_t(right - left), uint16_t(bottom - top)};
                }
        }
        // The ground around it.
        Rect      ground[4];
        size_t    n = 0;
        const int ax = area.x, ay = area.y, ar = area.right(), ab = area.bottom();
        if (pic.isEmpty()) {
                ground[n++] = area;
        } else {
                if (pic.y > ay) ground[n++] = Rect{uint16_t(ax), uint16_t(ay), uint16_t(ar - ax), uint16_t(pic.y - ay)};
                if (pic.bottom() < ab)
                        ground[n++] = Rect{uint16_t(ax), uint16_t(pic.bottom()), uint16_t(ar - ax),
                                           uint16_t(ab - pic.bottom())};
                if (pic.x > ax) ground[n++] = Rect{uint16_t(ax), pic.y, uint16_t(pic.x - ax), pic.height};
                if (pic.right() < ar)
                        ground[n++] = Rect{uint16_t(pic.right()), pic.y, uint16_t(ar - pic.right()), pic.height};
        }
        (void)painter.fill(Span<const Rect>(ground, n), Ground);
        // A word when there is no picture: why, or what to do.
        const char *say = _message[0] != '\0' ? _message
                          : _names.isEmpty()  ? "Open a picture (the toolbar's Open, or the file browser)"
                          : _loading          ? "Loading..."
                                              : nullptr;
        if (say != nullptr && pic.isEmpty()) {
                const uint16_t w = uint16_t(std::strlen(say) * 8);
                const int      tx = (ax + (area.width > w ? (area.width - w) / 2 : 0)) & ~3;
                const int      ty = ay + (area.height > textHeight() ? (area.height - textHeight()) / 2 : 0);
                (void)painter.text(tx, ty, StringView(say), font(), TextColor, Ground, area.width);
        }
        return;
}

void ImageWindow::showStatus() {
        char             buf[64];
        const StringView name = _index >= 0 ? nameOf(_names[size_t(_index)].view()) : StringView();
        _status->setText(0, name);
        if (_fileWidth != 0)
                std::snprintf(buf, sizeof(buf), "%u x %u", unsigned(_fileWidth), unsigned(_fileHeight));
        else
                buf[0] = '\0';
        _status->setText(1, StringView(buf));
        if (_shown.isValid())
                std::snprintf(buf, sizeof(buf), "%u%%", unsigned((_layout.zoom + 5) / 10));
        else
                buf[0] = '\0';
        _status->setText(2, StringView(buf));
        if (_index >= 0)
                std::snprintf(buf, sizeof(buf), "%d of %u", int(_index + 1), unsigned(_names.size()));
        else
                buf[0] = '\0';
        _status->setText(3, StringView(buf));
        char title[WindowManager::MaxTitle + 1];
        if (name.isEmpty())
                std::snprintf(title, sizeof(title), "%s", Title);
        else
                std::snprintf(title, sizeof(title), "%.*s - %s", int(name.size()), name.data(), Title);
        setTitle(StringView(title));
        return;
}

// ---- Asking the worker

// The worker decodes again only when the picture, its turns or the resolution wanted change.
void ImageWindow::request() {
        relayout();
        {
                Mutex::Locker lock(_lock, WaitForever);
                _request.generation = ++_generation;
                _request.index = _index;
                (void)_request.path.assign(pathOf(_index).view());
                _request.turns = _turns;
                _request.input = _in;
                const int dir = 1; // ahead: the next one
                _request.ahead = _index >= 0 && _index + dir < int32_t(_names.size()) ? _index + dir : -1;
                (void)_request.aheadPath.assign(pathOf(_request.ahead).view());
        }
        _wake.release();
        _message[0] = '\0';
        if (_shownIndex != _index) {
                // Loading: a word after a moment, the last picture kept meanwhile.
                if (_loadingTimer >= 0) application().stopTimer(_loadingTimer);
                const Result<int> t = application().startTimer(LoadingAfterMs, false, loadingDue, this);
                _loadingTimer = t.isOk() ? t.value() : -1;
        }
        showStatus();
        return;
}

void ImageWindow::loadingDue(void *arg) {
        auto *self = static_cast<ImageWindow *>(arg);
        self->_loadingTimer = -1;
        if (self->_shownIndex == self->_index) return;
        self->_loading = true;
        self->_shown.reset(); // the old picture would mislead
        if (self->_view != nullptr) self->_view->update();
        return;
}

void ImageWindow::resultDue(void *arg) {
        auto *self = static_cast<ImageWindow *>(arg);
        self->_posted = false;
        Made r;
        {
                Mutex::Locker lock(self->_lock, WaitForever);
                r.generation = self->_result.generation;
                r.index = self->_result.index;
                r.shown = std::move(self->_result.shown);
                r.layout = self->_result.layout;
                r.sourceWidth = self->_result.sourceWidth;
                r.sourceHeight = self->_result.sourceHeight;
                r.fileWidth = self->_result.fileWidth;
                r.fileHeight = self->_result.fileHeight;
                r.error = self->_result.error;
        }
        if (r.index != self->_index) return; // a picture since left
        self->_loading = false;
        if (self->_loadingTimer >= 0) {
                self->application().stopTimer(self->_loadingTimer);
                self->_loadingTimer = -1;
        }
        self->_error = r.error;
        self->_fileWidth = r.fileWidth;
        self->_fileHeight = r.fileHeight;
        if (r.error.isError()) {
                const StringView name = nameOf(self->_names[size_t(r.index)].view());
                const char      *why = r.error == Error::NotSupported ? "a kind not shown yet (progressive JPEG?)"
                                       : r.error == Error::OutOfRange ? "too big (image/decode-max)"
                                                                      : r.error.desc();
                std::snprintf(self->_message, sizeof(self->_message), "%.*s: %s", int(name.size()), name.data(), why);
                self->_shown.reset();
                self->_shownIndex = -1;
        } else {
                self->_in.sourceWidth = r.sourceWidth;
                self->_in.sourceHeight = r.sourceHeight;
                self->relayout();
                self->_shown = std::move(r.shown);
                self->_shownLayout = r.layout;
                self->_shownIndex = r.index;
        }
        self->showStatus();
        if (self->_view != nullptr) self->_view->update();
        return;
}

// ---- The worker

void ImageWindow::workerMain(void *arg) {
        static_cast<ImageWindow *>(arg)->work();
        return;
}

Error ImageWindow::decode(StringView path, int32_t index, uint8_t turns, const ViewLayout::Input &in, Decoded &out) {
        out.picture.reset();
        out.index = index;
        out.turns = turns;
        Images::FileBytes file;
        Error             err = file.read(path, Vfs::Context{});
        if (err.isError()) return err;
        // Fitted: a big JPEG decoded smaller, no smaller than the view needs; zoomed: whole.
        Images::Options o;
        o.format = PixelFormat::Bgr24;
        o.orient = true;
        if (in.fit) {
                // The box turns with the picture: its sides swapped for a quarter turn.
                o.boxWidth = (turns & 1) != 0 ? in.viewHeight : in.viewWidth;
                o.boxHeight = (turns & 1) != 0 ? in.viewWidth : in.viewHeight;
        }
        Images::Report report;
        err = Images::load(file.bytes(), o, out.picture, &report);
        if (err.isError()) return err;
        const ImageScaler::Size upright =
                ImageScaler::oriented(report.info.width, report.info.height, report.info.orientation);
        out.fileWidth = upright.width;
        out.fileHeight = upright.height;
        out.reduced = report.plan.shift != 0;
        // The user's turns.
        if ((turns & 3) != 0) {
                const ImageBuffer      &b = out.picture.image();
                const ImageScaler::Size s = ImageScaler::oriented(b.width, b.height, orientationOf(turns));
                Images::Picture         turned;
                err = turned.allocate(s.width, s.height, PixelFormat::Bgr24, MemSpace::Psram);
                if (err.isOk()) err = ImageScaler::orient(b, turned.image(), orientationOf(turns));
                if (err.isError()) return err;
                out.picture = std::move(turned);
                if ((turns & 1) != 0) std::swap(out.fileWidth, out.fileHeight);
        }
        return Error();
}

void ImageWindow::render(const Request &q, Made &r) {
        const ImageBuffer &src = _source.picture.image();
        ViewLayout::Input  in = q.input;
        in.sourceWidth = src.width;
        in.sourceHeight = src.height;
        const ViewLayout l = ViewLayout::make(in);
        r.layout = l;
        r.sourceWidth = src.width;
        r.sourceHeight = src.height;
        r.fileWidth = _source.fileWidth;
        r.fileHeight = _source.fileHeight;
        if (l.shownWidth == 0 || l.shownHeight == 0) {
                r.error = Error::Invalid;
                return;
        }
        // The part that shows, scaled (or copied, at its size) into a picture of its own.
        ImageBuffer crop = src;
        crop.pixels = src.row(l.cropY) + size_t(l.cropX) * 3;
        crop.width = l.cropWidth;
        crop.height = l.cropHeight;
        crop.capacity = 0;
        r.error = r.shown.allocate(l.shownWidth, l.shownHeight, PixelFormat::Bgr24, MemSpace::Psram);
        if (r.error.isOk()) r.error = ImageScaler::scale(crop, r.shown.image(), MemSpace::Psram);
        if (r.error.isError()) r.shown.reset();
        return;
}

void ImageWindow::work() {
        Request q;
        for (;;) {
                (void)_wake.acquire(WaitForever);
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        if (_quit) return;
                        q.generation = _request.generation;
                        q.index = _request.index;
                        (void)q.path.assign(_request.path.view());
                        q.turns = _request.turns;
                        q.input = _request.input;
                        q.ahead = _request.ahead;
                        (void)q.aheadPath.assign(_request.aheadPath.view());
                }
                if (q.index < 0) continue;
                Made r;
                r.generation = q.generation;
                r.index = q.index;
                // The picture: decoded already (now, or ahead), else decoded now. Zoomed past a reduced
                // decode wants the whole picture again.
                const bool wantsWhole = !q.input.fit;
                bool       have = _source.index == q.index && _source.turns == q.turns && _source.picture.isValid() &&
                            !(wantsWhole && _source.reduced);
                if (!have && _ahead.index == q.index && _ahead.turns == q.turns && _ahead.picture.isValid() &&
                    !(wantsWhole && _ahead.reduced)) {
                        std::swap(_source, _ahead);
                        have = true;
                }
                if (!have) r.error = decode(q.path.view(), q.index, q.turns, q.input, _source);
                if (r.error.isOk()) render(q, r);
                if (r.error.isError()) _source.index = -1;
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        _result.generation = r.generation;
                        _result.index = r.index;
                        _result.shown = std::move(r.shown);
                        _result.layout = r.layout;
                        _result.sourceWidth = r.sourceWidth;
                        _result.sourceHeight = r.sourceHeight;
                        _result.fileWidth = r.fileWidth;
                        _result.fileHeight = r.fileHeight;
                        _result.error = r.error;
                }
                if (!_posted.exchange(true) && application().post(resultDue, this).isError()) _posted = false;
                // Then the next one, unless asked something else meanwhile.
                bool newer;
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        newer = _request.generation != q.generation || _quit;
                }
                if (newer || q.ahead < 0 || (_ahead.index == q.ahead && _ahead.picture.isValid())) continue;
                if (decode(q.aheadPath.view(), q.ahead, 0, q.input, _ahead).isError()) _ahead.index = -1;
        }
}

// ---- Keys and tools

bool ImageWindow::keyUnused(const KeyEvent &event) {
        if (event.action == KeyEvent::Action::Release) return false;
        const bool zoomed = !_in.fit && pans();
        switch (event.usage) {
                case HidKey::Right:
                        if (zoomed)
                                panBy(PanStep, 0, true);
                        else
                                step(1);
                        return true;
                case HidKey::Left:
                        if (zoomed)
                                panBy(-PanStep, 0, true);
                        else
                                step(-1);
                        return true;
                case HidKey::Down:
                        if (zoomed) panBy(0, PanStep, true);
                        return true;
                case HidKey::Up:
                        if (zoomed) panBy(0, -PanStep, true);
                        return true;
                case HidKey::Space:
                case HidKey::PageDown: step(1); return true;
                case HidKey::Backspace:
                case HidKey::PageUp: step(-1); return true;
                case HidKey::Home: step(-int(_names.size())); return true;
                case HidKey::End: step(int(_names.size())); return true;
                case HidKey::Enter: toggleMaximized(); return true;
                case HidKey::Escape: // the program's one window: closing it ends the program
                        close();
                        application().quit(0);
                        return true;
                default: break;
        }
        // The rest by character, else by key (a keymap may give none): F fit, 1 actual size, + and -
        // zoom, R and Shift+R turn.
        constexpr uint8_t KeyF = HidKey::A + ('f' - 'a'), KeyR = HidKey::A + ('r' - 'a'), KeyEqual = 0x2e,
                          KeyMinus = 0x2d, KeypadMinus = 0x56, KeypadPlus = 0x57;
        const uint32_t c = event.codepoint;
        const uint8_t  u = event.usage;
        if (c == 'f' || c == 'F' || (c == 0 && u == KeyF)) {
                setZoom(true, ViewLayout::Actual);
        } else if (c == '1' || (c == 0 && u == HidKey::Digit1)) {
                setZoom(false, ViewLayout::Actual);
        } else if (c == '+' || c == '=' || u == KeypadPlus || (c == 0 && u == KeyEqual)) {
                setZoom(false, ViewLayout::step(_layout.zoom, true));
        } else if (c == '-' || u == KeypadMinus || (c == 0 && u == KeyMinus)) {
                setZoom(false, ViewLayout::step(_layout.zoom, false));
        } else if (c == 'r' || (c == 0 && u == KeyR && !event.hasShift())) {
                _turns = uint8_t((_turns + 1) & 3);
        } else if (c == 'R' || (c == 0 && u == KeyR && event.hasShift())) {
                _turns = uint8_t((_turns + 3) & 3);
        } else {
                return false;
        }
        request();
        return true;
}

void ImageWindow::toolClicked(void *arg, Button &button) {
        auto *self = static_cast<ImageWindow *>(arg);
        int   tool = -1;
        for (int i = 0; i < ToolMax; ++i)
                if (self->_toolButtons[i] == &button) tool = i;
        switch (tool) {
                case ToolOpen:
                        if (!self->_folder.isEmpty()) (void)self->_openDialog.setFolder(self->_folder.view());
                        (void)self->_openDialog.open();
                        break;
                case ToolPrevious: self->step(-1); break;
                case ToolNext: self->step(1); break;
                case ToolFit: self->setZoom(true, ViewLayout::Actual), self->request(); break;
                case ToolActual: self->setZoom(false, ViewLayout::Actual), self->request(); break;
                case ToolZoomOut:
                        self->setZoom(false, ViewLayout::step(self->_layout.zoom, false)), self->request();
                        break;
                case ToolZoomIn:
                        self->setZoom(false, ViewLayout::step(self->_layout.zoom, true)), self->request();
                        break;
                case ToolTurn: self->_turns = uint8_t((self->_turns + 1) & 3), self->request(); break;
                default: break;
        }
        return;
}

void ImageWindow::opened(void *arg, FileDialog &dialog, int result) {
        auto *self = static_cast<ImageWindow *>(arg);
        if (result != FileDialog::Accepted || dialog.count() == 0) return;
        const StringView path = dialog.path(0);
        self->open(Span<const StringView>(&path, 1));
        return;
}

BMDS_NAMESPACE_END
