/**
 * @file      graphics.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Graphics and input (devplan/scripting.md step 4): `Screen` and `Image`, both drawn on with
 * the same methods (lines, rectangles, circles, polygons, text, images), colours as 0xRRGGBB,
 * the keyboard and the mouse, and a frame loop for games (`Game`, `Screen::run`). A screen was
 * `Window` until the GUI's window took the name (devplan/guiprograms.md §13).
 *
 * A window is drawn immediately, as QBasic's screen was: drawing goes to a picture of the
 * script's (24-bit, so a colour read back is the colour drawn), and a thread of the runtime
 * presents what changed at the display's rate. That thread also reads the window's events
 * into a queue. Built for the host (no ANGEL_BMDS) a window is only its picture, with no
 * events: the corpus draws and reads pixels back.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <new>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <bmds/game.h>
#include "../../thirdparty/angelscript/add_on/scriptarray/scriptarray.h"
#include "angel.h"

#ifdef ANGEL_BMDS
#include <poll.h>
#include <bmds/sys.h>
#endif

namespace angel {

        namespace {

                constexpr uint32_t Format = BMDS_PIXEL_BGR24;
                constexpr uint32_t Bpp = 3;
                constexpr int      MaxSize = 4096;  // an image's side, at most
                constexpr int      QueueSize = 128; // events kept while the script doesn't read them
                constexpr int      MaxCatchUp = 5;  // ticks run at once after a stall

                int64_t monotonicNs() {
                        struct timespec t;
                        clock_gettime(CLOCK_MONOTONIC, &t);
                        return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
                }

                // ---- Pictures: what Window and Image share

                class Image;

                class Picture {
                        public:
                                virtual ~Picture() = default;

                                void addRef() { __atomic_add_fetch(&_refs, 1, __ATOMIC_RELAXED); }
                                void release() {
                                        if (__atomic_sub_fetch(&_refs, 1, __ATOMIC_ACQ_REL) == 0) delete this;
                                }

                                // Drawing: begin() gives the canvas to draw on (a window's locked and up to
                                // date; @p covered, if not null, is about to be painted whole), end() after.
                                virtual bmds_canvas_t *begin(const bmds_rect_t *covered) {
                                        (void)covered;
                                        return &canvas;
                                }
                                virtual void end() {}
                                // A solid rectangle (a window's by the 2D-DMA), between begin() and end().
                                virtual void fill(bmds_canvas_t *c, const bmds_rect_t &r, uint32_t pixel) {
                                        bmds_canvas_fill(c, r.x, r.y, int32_t(r.width), int32_t(r.height), pixel);
                                }
                                virtual void setClip(const bmds_rect_t *r) { bmds_canvas_set_clip(&canvas, r); }
                                // Draws @p image at @p x, @p y by other means than the CPU (a window's
                                // 2D-DMA). @return False if it can't: the caller copies it.
                                virtual bool drawImageFast(Image &image, int x, int y) {
                                        (void)image;
                                        (void)x;
                                        (void)y;
                                        return false;
                                }
                                // A window the user closed ends a script that doesn't watch for it.
                                virtual void checkClose() {}

                                bmds_game_image_t image() const {
                                        bmds_game_image_t i{};
                                        i.pixels = canvas.pixels;
                                        i.stride = canvas.stride;
                                        i.width = canvas.width;
                                        i.height = canvas.height;
                                        i.format = Format;
                                        i.flags = keyed ? BMDS_GAME_IMAGE_KEYED : 0;
                                        i.key = key;
                                        return i;
                                }

                                // An image's pixels; a window's size and clip (on the host, its picture too).
                                bmds_canvas_t canvas{};
                                bool          keyed = false;
                                uint32_t      key = 0;

                        private:
                                int _refs = 1;
                };

                // ---- Image

                class Image : public Picture {
                        public:
                                ~Image() override {
#ifdef ANGEL_BMDS
                                        if (_loaded.pixels != nullptr) bmds_game_image_free(&_loaded);
                                        bmds_mem_free(_own);
#else
                                        free(_own);
#endif
                                }

                                // Drawn on: a copy the 2D-DMA was given is out of date.
                                void end() override { ++generation; }

                                // A blank one (black), or nullptr (out of memory). On BMDS from
                                // bmds_mem_alloc(), so a window's 2D-DMA may copy from it.
                                static Image *make(int width, int height) {
                                        const size_t bytes = size_t(width) * height * Bpp;
#ifdef ANGEL_BMDS
                                        uint8_t *pixels = static_cast<uint8_t *>(
                                                bmds_mem_alloc(uint32_t(bytes), BMDS_MEM_ALIGN, 0));
                                        if (pixels != nullptr) memset(pixels, 0, bytes);
#else
                                        uint8_t *pixels = static_cast<uint8_t *>(calloc(bytes, 1));
#endif
                                        if (pixels == nullptr) return nullptr;
                                        Image *i = new Image();
                                        i->_own = pixels;
                                        bmds_canvas_init(&i->canvas, pixels, uint32_t(width) * Bpp, width, height,
                                                         Format);
                                        return i;
                                }

#ifdef ANGEL_BMDS
                                // A picture file (PNG, JPEG, BMP), transparent where it is; or nullptr and the error.
                                static Image *load(const char *path, int &error) {
                                        bmds_game_load_t how{};
                                        how.size = sizeof(how);
                                        how.format = Format;
                                        bmds_game_image_t loaded{};
                                        error = bmds_game_image_load(path, &how, &loaded);
                                        if (error != 0) return nullptr;
                                        Image *i = new Image();
                                        i->_loaded = loaded;
                                        bmds_canvas_init(&i->canvas, const_cast<uint8_t *>(loaded.pixels),
                                                         loaded.stride, loaded.width, loaded.height, Format);
                                        i->keyed = (loaded.flags & BMDS_GAME_IMAGE_KEYED) != 0;
                                        i->key = loaded.key;
                                        return i;
                                }
#endif

                                uint32_t generation = 0; // counts its changes

                        private:
                                Image() = default;

                                uint8_t          *_own = nullptr;
                                bmds_game_image_t _loaded{};
                };

                // ---- Events

                enum EventType {
                        KeyDown,
                        KeyUp,
                        MouseMove,
                        MouseDown,
                        MouseUp,
                        Wheel,
                        Close
                };

                // What the queue holds.
                struct RawEvent {
                                int  type;
                                char key[16];
                                char text[8];
                                int  x, y, button, wheel;
                                bool repeat;
                };

                // The script's Event: reference counted, its fields read-only properties.
                struct Event {
                                int    type = 0;
                                String key;
                                String text;
                                int    x = 0, y = 0, button = 0, wheel = 0;
                                bool   repeat = false;
                                int    refs = 1;

                                explicit Event(const RawEvent &e)
                                    : type(e.type), key(e.key), text(e.text), x(e.x), y(e.y), button(e.button),
                                      wheel(e.wheel), repeat(e.repeat) {}
                                void addRef() { ++refs; }
                                void release() {
                                        if (--refs == 0) delete this;
                                }
                };

                // HID keys (page 7) and their names.
                struct KeyName {
                                uint8_t     usage;
                                const char *name;
                };
                constexpr KeyName keyNames[] = {
                        {0x28, "Enter"},    {0x29, "Escape"},      {0x2a, "Backspace"},  {0x2b, "Tab"},
                        {0x2c, "Space"},    {0x2d, "-"},           {0x2e, "="},          {0x2f, "["},
                        {0x30, "]"},        {0x31, "\\"},          {0x33, ";"},          {0x34, "'"},
                        {0x35, "`"},        {0x36, ","},           {0x37, "."},          {0x38, "/"},
                        {0x39, "CapsLock"}, {0x3a, "F1"},          {0x3b, "F2"},         {0x3c, "F3"},
                        {0x3d, "F4"},       {0x3e, "F5"},          {0x3f, "F6"},         {0x40, "F7"},
                        {0x41, "F8"},       {0x42, "F9"},          {0x43, "F10"},        {0x44, "F11"},
                        {0x45, "F12"},      {0x46, "PrintScreen"}, {0x47, "ScrollLock"}, {0x48, "Pause"},
                        {0x49, "Insert"},   {0x4a, "Home"},        {0x4b, "PageUp"},     {0x4c, "Delete"},
                        {0x4d, "End"},      {0x4e, "PageDown"},    {0x4f, "Right"},      {0x50, "Left"},
                        {0x51, "Down"},     {0x52, "Up"},          {0x53, "NumLock"},    {0x58, "Enter"},
                        {0xe0, "Ctrl"},     {0xe1, "Shift"},       {0xe2, "Alt"},        {0xe3, "Super"},
                        {0xe4, "Ctrl"},     {0xe5, "Shift"},       {0xe6, "Alt"},        {0xe7, "Super"},
                };

#ifdef ANGEL_BMDS
                // A key's name: letters and digits as themselves (small), others by keyNames.
                void nameOf(uint32_t usage, char *out, size_t size) {
                        out[0] = 0;
                        if ((usage >> 16) != 7) return;
                        const uint32_t u = usage & 0xffff;
                        if (u >= 0x04 && u <= 0x1d) {
                                snprintf(out, size, "%c", char('a' + (u - 0x04)));
                        } else if (u >= 0x1e && u <= 0x27) {
                                snprintf(out, size, "%c", u == 0x27 ? '0' : char('1' + (u - 0x1e)));
                        } else if (u >= 0x59 && u <= 0x62) {
                                snprintf(out, size, "Keypad%c", u == 0x62 ? '0' : char('1' + (u - 0x59)));
                        } else {
                                for (const KeyName &k : keyNames)
                                        if (k.usage == u) {
                                                snprintf(out, size, "%s", k.name);
                                                return;
                                        }
                        }
                        return;
                }

#endif

                // The keys a name stands for (Shift: either), as a usage range; false if none.
                bool usagesOf(const char *name, uint32_t &first, uint32_t &second) {
                        first = second = 0;
                        if (name[0] != 0 && name[1] == 0) {
                                const char c = name[0];
                                if (c >= 'a' && c <= 'z')
                                        first = 0x04 + uint32_t(c - 'a');
                                else if (c >= 'A' && c <= 'Z')
                                        first = 0x04 + uint32_t(c - 'A');
                                else if (c >= '1' && c <= '9')
                                        first = 0x1e + uint32_t(c - '1');
                                else if (c == '0')
                                        first = 0x27;
                                else if (c == ' ')
                                        first = 0x2c;
                                if (first != 0) {
                                        second = first;
                                        return true;
                                }
                        }
                        for (const KeyName &k : keyNames)
                                if (strcasecmp(k.name, name) == 0) {
                                        if (first == 0)
                                                first = k.usage;
                                        else
                                                second = k.usage;
                                }
                        if (first == 0 && strncasecmp(name, "Keypad", 6) == 0 && name[6] >= '0' && name[6] <= '9' &&
                            name[7] == 0)
                                first = name[6] == '0' ? 0x62 : 0x59 + uint32_t(name[6] - '1');
                        if (second == 0) second = first;
                        return first != 0;
                }

#ifdef ANGEL_BMDS
                void utf8(uint32_t c, char *out) {
                        if (c < 0x80) {
                                out[0] = char(c);
                                out[1] = 0;
                        } else if (c < 0x800) {
                                out[0] = char(0xc0 | (c >> 6));
                                out[1] = char(0x80 | (c & 0x3f));
                                out[2] = 0;
                        } else if (c < 0x10000) {
                                out[0] = char(0xe0 | (c >> 12));
                                out[1] = char(0x80 | ((c >> 6) & 0x3f));
                                out[2] = char(0x80 | (c & 0x3f));
                                out[3] = 0;
                        } else {
                                out[0] = char(0xf0 | (c >> 18));
                                out[1] = char(0x80 | ((c >> 12) & 0x3f));
                                out[2] = char(0x80 | ((c >> 6) & 0x3f));
                                out[3] = char(0x80 | (c & 0x3f));
                                out[4] = 0;
                        }
                        return;
                }

#endif

                // ---- Window

                class Window;
                Window *openWindows[8];

                // A script that calls frame() is shown only by frame() (and when it waits for input),
                // so an animation never shows a frame half drawn; what it draws and leaves unshown
                // this long (it stopped calling frame()) is shown anyway. A script that never calls
                // frame() is shown as it draws, as QBasic's screen was. The time runs from the first
                // drawing not yet shown, not from the last frame(): a script that waited a while for
                // a key and then redraws mustn't have its clear() shown before the rest.
                constexpr int64_t FramePacedNs = 100000000LL;

                // On BMDS a window is drawn straight into its surface's buffers: one is held to draw
                // on, the presenter shows it and takes the next, which is brought up to date (by the
                // 2D-DMA, from the newest shown) the first time it is drawn on, less what that drawing
                // paints whole (clear() brings nothing over). On the host a window is a picture.
                constexpr uint32_t Buffers = 3;

                // A 2D-DMA job costs ~150 µs whatever its size (docs/platforms/esp32p4.md): a fill
                // smaller than this many bytes is the CPU's (a 74 x 74 square, at 3 bytes a pixel).
                constexpr uint64_t DmaFillBytes = 16 * 1024;

                class Window : public Picture {
                        public:
                                ~Window() override;

                                // Opens it, or returns nullptr with the message raised.
                                static Window *open(const char *title, int width, int height, bool fullScreen);

                                bmds_canvas_t *begin(const bmds_rect_t *covered) override;
                                void           end() override;
                                void           fill(bmds_canvas_t *c, const bmds_rect_t &r, uint32_t pixel) override;
                                void           setClip(const bmds_rect_t *r) override;
                                void           checkClose() override {
                                        if (_closed && !_closeWatched) {
                                                restoreTerminal();
                                                exit(0);
                                        }
                                }

                                bool closed() {
                                        _closeWatched = true;
                                        return _closed;
                                }
                                void setTitle(const char *title);
                                void showPointer(bool visible);
                                void frame();
                                // The next event, waiting (or not) for one; false if none.
                                bool next(RawEvent &e, bool wait, bool keysOnly);
                                bool keyDown(const char *name);
                                int  mouseX() const { return _mouseX; }
                                int  mouseY() const { return _mouseY; }
                                bool mouseDown(int button) const {
                                        return button >= 1 && button <= 8 && (_buttons & (1u << (button - 1))) != 0;
                                }
                                int takeWheel() {
                                        const int w = _wheel;
                                        _wheel = 0;
                                        return w;
                                }
                                void watchClose() { _closeWatched = true; }
                                bool closeWatched() const { return _closeWatched; }
                                void setCloseWatched(bool watched) { _closeWatched = watched; }
                                bool isClosed() const { return _closed; }
                                // At the script's end: shown until the user closes it.
                                void waitUntilClosed();
                                // Waits until an event is waiting, the window closes, or the monotonic
                                // clock reaches @p deadlineNs (run()'s next tick), whichever is first.
                                void waitForEvent(int64_t deadlineNs);

                                bool running = false;

                        private:
                                Window() = default;
#ifdef ANGEL_BMDS
                                static void *presenter(void *arg);
                                void         wake();
                                void         requestPresent();
                                void         readEvents();
                                void         present();
                                void         push(const RawEvent &e);
                                void         catchUp(const bmds_rect_t *covered);
                                bool dma(uint32_t source, uint32_t from, const bmds_rect_t *rects, uint32_t count);
                                bool         drawImageFast(Image &image, int x, int y) override;
                                int          registeredSlot(Image &image); // its slot for the 2D-DMA, or -1
#else
                                uint8_t *_picture = nullptr;
#endif

                                pthread_mutex_t _mutex = PTHREAD_MUTEX_INITIALIZER;
                                pthread_cond_t _changed = PTHREAD_COND_INITIALIZER; // shown, a buffer, an event, closed
                                uint32_t       _generation = 0;                     // drawings so far
                                uint32_t       _shown = 0;                          // of them, shown
                                bool           _closed = false;
                                bool           _closeWatched = false;
                                RawEvent       _queue[QueueSize];
                                int            _head = 0, _count = 0;
                                uint8_t        _keys[32] = {};
                                int            _mouseX = 0, _mouseY = 0;
                                uint32_t       _buttons = 0;
                                int            _wheel = 0;
#ifdef ANGEL_BMDS
                                int           _fd = -1;
                                int           _wakeRead = -1, _wakeWrite = -1;
                                bool          _wakePending = false;
                                bool          _presentRequested = false;
                                int64_t       _lastFrameNs = 0; // the script's last frame() (0: none yet)
                                int64_t       _unshownNs = 0;   // the first drawing not yet shown (0: none)
                                bool          _quit = false;
                                bool          _noDma = false;
                                // Images registered for the 2D-DMA to copy from: each held, with its
                                // generation as registered and when it was last drawn (to replace the
                                // least recently used).
                                struct DmaImage {
                                                Image   *image = nullptr;
                                                uint32_t generation = 0;
                                                uint32_t used = 0;
                                };
                                DmaImage      _dmaImages[BMDS_SURFACE_IMAGES];
                                uint32_t      _dmaClock = 0;
                                bool          _noImageDma = false;
                                int64_t       _closeDeadlineNs = 0;
                                pthread_t     _thread{};
                                bool          _threadStarted = false;
                                bmds_canvas_t _buffers[Buffers]{}; // their damage: drawn since shown
                                bmds_canvas_t _missing[Buffers]{}; // their damage: what each lacks of the newest
                                int32_t       _current = -1;       // held, to draw on (-1: being swapped)
                                int32_t       _newest = -1;        // the last shown
#endif
                };

                Window::~Window() {
#ifdef ANGEL_BMDS
                        if (_threadStarted) {
                                pthread_mutex_lock(&_mutex);
                                _quit = true;
                                pthread_mutex_unlock(&_mutex);
                                wake();
                                pthread_join(_thread, nullptr);
                        }
                        if (_fd >= 0) close(_fd);
                        for (DmaImage &d : _dmaImages)
                                if (d.image != nullptr) d.image->release();
                        if (_wakeRead >= 0) close(_wakeRead);
                        if (_wakeWrite >= 0) close(_wakeWrite);
#else
                        free(_picture);
#endif
                        for (Window *&w : openWindows)
                                if (w == this) w = nullptr;
                }

                Window *Window::open(const char *title, int width, int height, bool fullScreen) {
                        if (width < 1 || height < 1 || width > 1280 || height > 720) {
                                raise("A window is 1 to 1280 pixels across and 1 to 720 down, not %d x %d", width,
                                      height);
                                return nullptr;
                        }
                        int slot = -1;
                        for (int i = 0; i < 8; ++i)
                                if (openWindows[i] == nullptr) slot = i;
                        if (slot < 0) {
                                raise("Too many windows (at most 8)");
                                return nullptr;
                        }
                        Window *w = new Window();
#ifdef ANGEL_BMDS
                        bmds_surface_config_t config{};
                        config.size = sizeof(config);
                        config.width = uint32_t(width + 3) & ~3u; // the surface's: a multiple of 4
                        config.height = uint32_t(height);
                        config.format = Format;
                        config.buffers = Buffers;
                        config.flags = fullScreen ? BMDS_SURFACE_FULL_SCREEN : 0;
                        config.title = title[0] != 0 ? title : nullptr;
                        w->_fd = bmds_surface_open(&config);
                        if (w->_fd < 0) {
                                const int e = -w->_fd;
                                w->_fd = -1;
                                delete w;
                                if (e == BMDS_ENODEV)
                                        raise("There is no screen to open a window on");
                                else
                                        raise("Can't open a window: %s", strerror(e));
                                return nullptr;
                        }
                        fcntl(w->_fd, F_SETFD, FD_CLOEXEC);
                        uint32_t buffers = 0;
                        for (uint32_t i = 0; i < Buffers; ++i) {
                                bmds_surface_buffer_t b{};
                                b.size = sizeof(b);
                                b.index = i;
                                if (bmds_surface_buffer(w->_fd, &b) != 0) break;
                                bmds_canvas_init(&w->_buffers[i], b.pixels, b.stride, width, height, Format);
                                bmds_canvas_init(&w->_missing[i], b.pixels, b.stride, width, height, Format);
                                ++buffers;
                        }
                        int pipeFds[2];
                        if (buffers != Buffers || pipe2(pipeFds, O_CLOEXEC | O_NONBLOCK) != 0) {
                                delete w;
                                raise("Can't open a window: %s", strerror(errno));
                                return nullptr;
                        }
                        bmds_canvas_init(&w->canvas, w->_buffers[0].pixels, w->_buffers[0].stride, width, height,
                                         Format);
                        w->_wakeRead = pipeFds[0];
                        w->_wakeWrite = pipeFds[1];
                        w->_current = bmds_surface_acquire(w->_fd, -1);
                        pthread_attr_t attr;
                        pthread_attr_init(&attr);
                        pthread_attr_setstacksize(&attr, 16384);
                        const int r = w->_current < 0 ? -w->_current : pthread_create(&w->_thread, &attr, presenter, w);
                        pthread_attr_destroy(&attr);
                        if (r != 0) {
                                delete w;
                                raise("Can't open a window: %s", strerror(r));
                                return nullptr;
                        }
                        w->_threadStarted = true;
#else
                        (void)title;
                        (void)fullScreen;
                        w->_picture = static_cast<uint8_t *>(calloc(size_t(width) * height, Bpp));
                        if (w->_picture == nullptr) {
                                delete w;
                                raise("Out of memory for a %d x %d window", width, height);
                                return nullptr;
                        }
                        bmds_canvas_init(&w->canvas, w->_picture, uint32_t(width) * Bpp, width, height, Format);
#endif
                        openWindows[slot] = w;
                        return w;
                }

#ifdef ANGEL_BMDS
                bmds_canvas_t *Window::begin(const bmds_rect_t *covered) {
                        pthread_mutex_lock(&_mutex);
                        while (_current < 0 && !_quit) pthread_cond_wait(&_changed, &_mutex);
                        catchUp(covered);
                        return &_buffers[_current];
                }

                void Window::end() {
                        ++_generation;
                        // The presenter is woken to show it at once (a script that doesn't call
                        // frame()), or, paced by frame(), only by the first drawing not yet shown, to
                        // start its clock for showing it anyway: not by every call (a game's frame is
                        // dozens of them).
                        bool first = false;
                        if (_unshownNs == 0 && _current >= 0 && _buffers[_current].damage_count != 0) {
                                _unshownNs = monotonicNs();
                                first = true;
                        }
                        const bool paced = _lastFrameNs != 0;
                        pthread_mutex_unlock(&_mutex);
                        if (!paced || first) wake();
                        return;
                }

                // Copies or fills @p count rectangles of the held buffer by the 2D-DMA (false: the
                // kernel has none; the caller uses the CPU).
                bool Window::dma(uint32_t source, uint32_t from, const bmds_rect_t *rects, uint32_t count) {
                        if (_noDma || count == 0) return !_noDma;
                        bmds_surface_piece_t pieces[BMDS_CANVAS_MAX_DAMAGE];
                        for (uint32_t i = 0; i < count; ++i)
                                pieces[i] = {rects[i].x,      rects[i].y, rects[i].width,
                                             rects[i].height, rects[i].x, rects[i].y};
                        const bmds_surface_blit_t blit = {sizeof(blit), uint32_t(_current), source, from, count,
                                                          pieces};
                        if (bmds_surface_control(_fd, BMDS_SURFACE_BLIT, const_cast<bmds_surface_blit_t *>(&blit),
                                                 sizeof(blit)) == 0)
                                return true;
                        _noDma = true;
                        return false;
                }

                // The held buffer brought up to the newest shown, but for what @p covered paints.
                void Window::catchUp(const bmds_rect_t *covered) {
                        bmds_canvas_t &missing = _missing[_current];
                        if (missing.damage_count == 0) return;
                        if (covered != nullptr) {
                                // What the drawing paints whole needn't be brought over.
                                uint32_t kept = 0;
                                for (uint32_t i = 0; i < missing.damage_count; ++i) {
                                        const bmds_rect_t &m = missing.damage[i];
                                        const bool         inside =
                                                m.x >= covered->x && m.y >= covered->y &&
                                                int64_t(m.x) + m.width <= int64_t(covered->x) + covered->width &&
                                                int64_t(m.y) + m.height <= int64_t(covered->y) + covered->height;
                                        if (!inside) missing.damage[kept++] = m;
                                }
                                missing.damage_count = kept;
                        }
                        if (missing.damage_count != 0 && _newest >= 0 && _newest != _current &&
                            !dma(BMDS_BLIT_BUFFER, uint32_t(_newest), missing.damage, missing.damage_count))
                                bmds_canvas_copy(&_buffers[_current], &_buffers[_newest], missing.damage,
                                                 missing.damage_count);
                        bmds_canvas_damage_clear(&missing);
                        return;
                }

                void Window::fill(bmds_canvas_t *c, const bmds_rect_t &r, uint32_t pixel) {
                        // Cut to the clip, then one piece for the 2D-DMA.
                        const int64_t x0 = r.x > c->clip.x ? r.x : c->clip.x, y0 = r.y > c->clip.y ? r.y : c->clip.y;
                        const int64_t rx1 = int64_t(r.x) + r.width, ry1 = int64_t(r.y) + r.height;
                        const int64_t cx1 = int64_t(c->clip.x) + c->clip.width,
                                      cy1 = int64_t(c->clip.y) + c->clip.height;
                        const int64_t x1 = rx1 < cx1 ? rx1 : cx1, y1 = ry1 < cy1 ? ry1 : cy1;
                        if (x1 <= x0 || y1 <= y0) return;
                        const bmds_rect_t piece = {int32_t(x0), int32_t(y0), uint32_t(x1 - x0), uint32_t(y1 - y0)};
                        if (uint64_t(piece.width) * piece.height * Bpp >= DmaFillBytes &&
                            dma(BMDS_BLIT_FILL, pixel, &piece, 1))
                                bmds_canvas_damage(c, &piece);
                        else
                                bmds_canvas_fill(c, piece.x, piece.y, int32_t(piece.width), int32_t(piece.height),
                                                 pixel);
                        return;
                }

                // A large opaque image by the 2D-DMA: registered with the surface (its cache written
                // back once, again only after it changes), then copied in one job. A full-window
                // image by the CPU was ~30 ms (PSRAM to PSRAM); small ones and see-through ones stay
                // the CPU's (the 2D-DMA has no key).
                bool Window::drawImageFast(Image &image, int x, int y) {
                        const bmds_canvas_t &from = image.canvas;
                        if (_noDma || _noImageDma || image.keyed ||
                            uint64_t(from.width) * from.height * Bpp < DmaFillBytes)
                                return false;
                        pthread_mutex_lock(&_mutex);
                        const bmds_rect_t clip = canvas.clip;
                        pthread_mutex_unlock(&_mutex);
                        const int64_t x0 = x > clip.x ? x : clip.x, y0 = y > clip.y ? y : clip.y;
                        const int64_t ix1 = int64_t(x) + from.width, iy1 = int64_t(y) + from.height;
                        const int64_t cx1 = int64_t(clip.x) + clip.width, cy1 = int64_t(clip.y) + clip.height;
                        const int64_t x1 = ix1 < cx1 ? ix1 : cx1, y1 = iy1 < cy1 ? iy1 : cy1;
                        if (x1 <= x0 || y1 <= y0) return true; // nothing of it shows
                        const int slot = registeredSlot(image);
                        if (slot < 0) return false;
                        const bmds_rect_t    r = {int32_t(x0), int32_t(y0), uint32_t(x1 - x0), uint32_t(y1 - y0)};
                        bmds_canvas_t       *c = begin(&r);
                        bmds_surface_piece_t piece = {r.x, r.y, r.width, r.height, int32_t(x0 - x), int32_t(y0 - y)};
                        const bmds_surface_blit_t blit = {
                                sizeof(blit), uint32_t(_current), BMDS_BLIT_IMAGE, uint32_t(slot), 1, &piece};
                        const bool done =
                                bmds_surface_control(_fd, BMDS_SURFACE_BLIT, const_cast<bmds_surface_blit_t *>(&blit),
                                                     sizeof(blit)) == 0;
                        if (done) {
                                bmds_canvas_damage(c, &r);
                        } else {
                                _noImageDma = true;
                                const bmds_game_image_t img = image.image();
                                bmds_canvas_blit(c, x, y, &img, nullptr);
                        }
                        end();
                        return true;
                }

                int Window::registeredSlot(Image &image) {
                        int slot = -1;
                        for (uint32_t i = 0; i < BMDS_SURFACE_IMAGES; ++i)
                                if (_dmaImages[i].image == &image) slot = int(i);
                        if (slot < 0) {
                                // A free slot, else the least recently used.
                                slot = 0;
                                for (uint32_t i = 0; i < BMDS_SURFACE_IMAGES; ++i) {
                                        if (_dmaImages[i].image == nullptr) {
                                                slot = int(i);
                                                break;
                                        }
                                        if (_dmaImages[i].used < _dmaImages[slot].used) slot = int(i);
                                }
                                DmaImage &d = _dmaImages[slot];
                                if (d.image != nullptr) d.image->release();
                                image.addRef();
                                d.image = &image;
                                d.generation = image.generation - 1; // registered below
                        }
                        DmaImage &d = _dmaImages[slot];
                        d.used = ++_dmaClock;
                        if (d.generation != image.generation) {
                                const bmds_canvas_t       &from = image.canvas;
                                const bmds_surface_image_t im = {
                                        sizeof(im),           uint32_t(slot),        from.pixels,
                                        uint32_t(from.width), uint32_t(from.height), uint32_t(from.stride)};
                                if (bmds_surface_control(_fd, BMDS_SURFACE_IMAGE,
                                                         const_cast<bmds_surface_image_t *>(&im), sizeof(im)) != 0) {
                                        _noImageDma = true; // the kernel won't: the CPU from now on
                                        return -1;
                                }
                                d.generation = image.generation;
                        }
                        return slot;
                }

                void Window::setClip(const bmds_rect_t *r) {
                        pthread_mutex_lock(&_mutex);
                        bmds_canvas_set_clip(&canvas, r);
                        for (bmds_canvas_t &b : _buffers) bmds_canvas_set_clip(&b, r);
                        pthread_mutex_unlock(&_mutex);
                        return;
                }

                void Window::wake() {
                        pthread_mutex_lock(&_mutex);
                        const bool send = !_wakePending;
                        _wakePending = true;
                        pthread_mutex_unlock(&_mutex);
                        if (send) {
                                const char b = 1;
                                (void)write(_wakeWrite, &b, 1);
                        }
                        return;
                }

                void Window::requestPresent() {
                        pthread_mutex_lock(&_mutex);
                        _presentRequested = true;
                        pthread_mutex_unlock(&_mutex);
                        wake();
                        return;
                }
#else
                bmds_canvas_t *Window::begin(const bmds_rect_t *covered) {
                        (void)covered;
                        pthread_mutex_lock(&_mutex);
                        return &canvas;
                }

                void Window::end() {
                        ++_generation;
                        _shown = _generation; // nothing to wait for
                        pthread_mutex_unlock(&_mutex);
                        return;
                }

                void Window::fill(bmds_canvas_t *c, const bmds_rect_t &r, uint32_t pixel) {
                        bmds_canvas_fill(c, r.x, r.y, int32_t(r.width), int32_t(r.height), pixel);
                        return;
                }

                void Window::setClip(const bmds_rect_t *r) {
                        bmds_canvas_set_clip(&canvas, r);
                        return;
                }
#endif

                void Window::setTitle(const char *title) {
#ifdef ANGEL_BMDS
                        bmds_surface_control(_fd, BMDS_SURFACE_TITLE, const_cast<char *>(title),
                                             uint32_t(strlen(title)));
#else
                        (void)title;
#endif
                        return;
                }

                void Window::showPointer(bool visible) {
#ifdef ANGEL_BMDS
                        uint32_t mode = visible ? BMDS_POINTER_NORMAL : BMDS_POINTER_HIDDEN;
                        bmds_surface_control(_fd, BMDS_SURFACE_POINTER, &mode, sizeof(mode));
#else
                        (void)visible;
#endif
                        return;
                }

                // Shows what was drawn, and waits for the display's next frame.
                void Window::frame() {
                        fflush(stdout);
#ifdef ANGEL_BMDS
                        pthread_mutex_lock(&_mutex);
                        const uint32_t target = _generation;
                        const bool     drawn = _shown != target;
                        _lastFrameNs = monotonicNs();
                        pthread_mutex_unlock(&_mutex);
                        if (drawn) {
                                requestPresent();
                                pthread_mutex_lock(&_mutex);
                                while (_shown != target && !_closed && !_quit) pthread_cond_wait(&_changed, &_mutex);
                                pthread_mutex_unlock(&_mutex);
                        } else {
                                bmds_surface_info_t info{};
                                info.size = sizeof(info);
                                if (bmds_surface_control(_fd, BMDS_SURFACE_INFO, &info, sizeof(info)) == 0) {
                                        bmds_surface_wait_t wait{sizeof(bmds_surface_wait_t), info.frames + 1, 100};
                                        bmds_surface_control(_fd, BMDS_SURFACE_WAIT, &wait, sizeof(wait));
                                }
                        }
#endif
                        checkClose();
                        return;
                }

                bool Window::next(RawEvent &e, bool wait, bool keysOnly) {
                        pthread_mutex_lock(&_mutex);
                        for (;;) {
                                while (_count > 0) {
                                        e = _queue[_head];
                                        _head = (_head + 1) % QueueSize;
                                        --_count;
                                        if (!keysOnly || e.type == KeyDown) {
                                                pthread_mutex_unlock(&_mutex);
                                                return true;
                                        }
                                }
                                if (!wait || _closed) break;
#ifdef ANGEL_BMDS
                                // Waiting for input: what was drawn is shown first.
                                if (_shown != _generation && !_presentRequested) {
                                        _presentRequested = true;
                                        pthread_mutex_unlock(&_mutex);
                                        wake();
                                        pthread_mutex_lock(&_mutex);
                                        continue;
                                }
                                pthread_cond_wait(&_changed, &_mutex);
#else
                                break; // no events on the host
#endif
                        }
                        pthread_mutex_unlock(&_mutex);
                        return false;
                }

                bool Window::keyDown(const char *name) {
                        uint32_t a, b;
                        if (!usagesOf(name, a, b)) return false;
                        return (_keys[a >> 3] & (1u << (a & 7))) != 0 || (_keys[b >> 3] & (1u << (b & 7))) != 0;
                }

                void Window::waitForEvent(int64_t deadlineNs) {
#ifdef ANGEL_BMDS
                        pthread_mutex_lock(&_mutex);
                        // Waits of a tenth of a second at most, on the wall clock (the condition's):
                        // a change of the time can't make one much longer.
                        while (_count == 0 && !_closed) {
                                const int64_t left = deadlineNs - monotonicNs();
                                if (left <= 0) break;
                                const int64_t   wait = left < 100000000LL ? left : 100000000LL;
                                struct timespec until;
                                clock_gettime(CLOCK_REALTIME, &until);
                                const int64_t ns = int64_t(until.tv_nsec) + wait;
                                until.tv_sec += time_t(ns / 1000000000LL);
                                until.tv_nsec = long(ns % 1000000000LL);
                                pthread_cond_timedwait(&_changed, &_mutex, &until);
                        }
                        pthread_mutex_unlock(&_mutex);
#else
                        // No events on the host: the time only.
                        const int64_t left = deadlineNs - monotonicNs();
                        if (left > 0) {
                                struct timespec t = {time_t(left / 1000000000), long(left % 1000000000)};
                                nanosleep(&t, nullptr);
                        }
#endif
                        return;
                }

                void Window::waitUntilClosed() {
#ifdef ANGEL_BMDS
                        pthread_mutex_lock(&_mutex);
                        while (!_closed) pthread_cond_wait(&_changed, &_mutex);
                        pthread_mutex_unlock(&_mutex);
#endif
                        return;
                }

#ifdef ANGEL_BMDS
                void Window::push(const RawEvent &e) {
                        // Pointer motion coalesced: only the latest position waits in the queue.
                        if (e.type == MouseMove && _count > 0) {
                                RawEvent &last = _queue[(_head + _count - 1) % QueueSize];
                                if (last.type == MouseMove) {
                                        last = e;
                                        return;
                                }
                        }
                        if (_count == QueueSize) { // full: the oldest goes
                                _head = (_head + 1) % QueueSize;
                                --_count;
                        }
                        _queue[(_head + _count) % QueueSize] = e;
                        ++_count;
                        return;
                }

                void Window::readEvents() {
                        bmds_surface_event_t events[16];
                        const ssize_t        n = read(_fd, events, sizeof(events));
                        if (n <= 0) return;
                        pthread_mutex_lock(&_mutex);
                        for (size_t i = 0; i < size_t(n) / sizeof(events[0]); ++i) {
                                const bmds_surface_event_t &s = events[i];
                                RawEvent                    e{};
                                switch (s.type) {
                                        case BMDS_EVENT_KEY: {
                                                const uint32_t usage = s.data[0];
                                                const bool down = (s.flags & (BMDS_KEY_DOWN | BMDS_KEY_REPEAT)) != 0;
                                                if ((usage >> 16) == 7 && (usage & 0xffff) < 256) {
                                                        const uint32_t u = usage & 0xff;
                                                        if (down)
                                                                _keys[u >> 3] |= uint8_t(1u << (u & 7));
                                                        else
                                                                _keys[u >> 3] &= uint8_t(~(1u << (u & 7)));
                                                }
                                                e.type = down ? KeyDown : KeyUp;
                                                e.repeat = (s.flags & BMDS_KEY_REPEAT) != 0;
                                                nameOf(usage, e.key, sizeof(e.key));
                                                if (down && s.data[2] >= 32 && s.data[2] != 127)
                                                        utf8(s.data[2], e.text);
                                                e.x = _mouseX;
                                                e.y = _mouseY;
                                                push(e);
                                                break;
                                        }
                                        case BMDS_EVENT_POINTER: {
                                                const int32_t  x = int32_t(s.data[0]), y = int32_t(s.data[1]);
                                                const uint32_t buttons = s.data[2];
                                                if (x != BMDS_POINTER_GONE) {
                                                        _mouseX = x;
                                                        _mouseY = y;
                                                }
                                                const uint32_t changed = buttons ^ _buttons;
                                                _buttons = buttons;
                                                e.x = _mouseX;
                                                e.y = _mouseY;
                                                if (changed == 0) {
                                                        e.type = MouseMove;
                                                        push(e);
                                                }
                                                for (int b = 0; b < 8; ++b)
                                                        if ((changed & (1u << b)) != 0) {
                                                                e.type = (buttons & (1u << b)) != 0 ? MouseDown
                                                                                                    : MouseUp;
                                                                e.button = b + 1;
                                                                push(e);
                                                        }
                                                break;
                                        }
                                        case BMDS_EVENT_WHEEL:
                                                e.type = Wheel;
                                                e.wheel = int32_t(s.data[0]);
                                                e.x = _mouseX;
                                                e.y = _mouseY;
                                                _wheel += e.wheel;
                                                push(e);
                                                break;
                                        case BMDS_EVENT_CLOSE:
                                                _closed = true;
                                                e.type = Close;
                                                push(e);
                                                if (!_closeWatched) _closeDeadlineNs = monotonicNs() + 1000000000LL;
                                                break;
                                        default: break;
                                }
                        }
                        pthread_cond_broadcast(&_changed);
                        pthread_mutex_unlock(&_mutex);
                        return;
                }

                // Shows the held buffer if it was drawn on, and takes the next to draw on.
                void Window::present() {
                        pthread_mutex_lock(&_mutex);
                        const uint32_t generation = _generation;
                        if (_current < 0 || _buffers[_current].damage_count == 0) {
                                _shown = generation; // drawn nothing that shows
                                _unshownNs = 0;
                                pthread_cond_broadcast(&_changed);
                                pthread_mutex_unlock(&_mutex);
                                return;
                        }
                        bmds_canvas_t &held = _buffers[_current];
                        bmds_rect_t    damage[BMDS_CANVAS_MAX_DAMAGE];
                        const uint32_t count = held.damage_count;
                        memcpy(damage, held.damage, sizeof(bmds_rect_t) * count);
                        bmds_canvas_damage_clear(&held);
                        bmds_surface_present_t p{};
                        p.size = sizeof(p);
                        p.index = uint32_t(_current);
                        p.damage = damage;
                        p.damage_count = count;
                        const int32_t frame = bmds_surface_present(_fd, &p);
                        _newest = _current;
                        for (uint32_t b = 0; b < Buffers; ++b)
                                if (int32_t(b) != _newest)
                                        for (uint32_t i = 0; i < count; ++i)
                                                bmds_canvas_damage(&_missing[b], &damage[i]);
                        // The next buffer (the third is free at once; the drawing waits only if not).
                        _current = bmds_surface_acquire(_fd, 0);
                        if (_current < 0) {
                                pthread_mutex_unlock(&_mutex);
                                const int32_t next = bmds_surface_acquire(_fd, 100);
                                pthread_mutex_lock(&_mutex);
                                _current = next;
                        }
                        _shown = generation;
                        _unshownNs = 0;
                        pthread_cond_broadcast(&_changed);
                        pthread_mutex_unlock(&_mutex);
                        // Paced on the display: the next present after this one's frame has begun.
                        if (frame >= 0) {
                                bmds_surface_wait_t wait{sizeof(bmds_surface_wait_t), uint32_t(frame), 100};
                                bmds_surface_control(_fd, BMDS_SURFACE_WAIT, &wait, sizeof(wait));
                        }
                        return;
                }

                void *Window::presenter(void *arg) {
                        Window *w = static_cast<Window *>(arg);
                        for (;;) {
                                struct pollfd fds[2] = {{w->_fd, POLLIN, 0}, {w->_wakeRead, POLLIN, 0}};
                                pthread_mutex_lock(&w->_mutex);
                                const int64_t deadline = w->_closeDeadlineNs;
                                // Still no buffer to draw on: try again soon.
                                int timeout = w->_current < 0 ? 10 : -1;
                                // Drawn, but paced by frame(): shown by itself if it stays unshown.
                                if (w->_current >= 0 && w->_buffers[w->_current].damage_count != 0 &&
                                    w->_lastFrameNs != 0 && w->_unshownNs != 0 && !w->_presentRequested) {
                                        const int64_t left = w->_unshownNs + FramePacedNs - monotonicNs();
                                        timeout = left <= 0 ? 0 : int(left / 1000000) + 1;
                                }
                                pthread_mutex_unlock(&w->_mutex);
                                if (deadline != 0) {
                                        const int64_t left = deadline - monotonicNs();
                                        const int     ms = left <= 0 ? 0 : int(left / 1000000) + 1;
                                        if (timeout < 0 || ms < timeout) timeout = ms;
                                }
                                if (poll(fds, 2, timeout) < 0 && errno != EINTR) break;
                                if ((fds[0].revents & POLLIN) != 0) w->readEvents();
                                if ((fds[1].revents & POLLIN) != 0) {
                                        char drain[16];
                                        while (read(w->_wakeRead, drain, sizeof(drain)) > 0) {}
                                }
                                pthread_mutex_lock(&w->_mutex);
                                w->_wakePending = false;
                                const bool quit = w->_quit;
                                // Closed, and the script doesn't watch for it: after a second, the end.
                                const bool give = w->_closeDeadlineNs != 0 && !w->_closeWatched &&
                                                  monotonicNs() >= w->_closeDeadlineNs;
                                if (w->_closeWatched) w->_closeDeadlineNs = 0;
                                if (w->_current < 0) {
                                        const int32_t next = bmds_surface_acquire(w->_fd, 0);
                                        if (next >= 0) {
                                                w->_current = next;
                                                pthread_cond_broadcast(&w->_changed);
                                        }
                                }
                                const bool paced = w->_lastFrameNs != 0 &&
                                                   (w->_unshownNs == 0 || monotonicNs() - w->_unshownNs < FramePacedNs);
                                const bool show = w->_presentRequested || !paced;
                                w->_presentRequested = false;
                                pthread_mutex_unlock(&w->_mutex);
                                if (quit) break;
                                if (give) _exit(0);
                                if (show) w->present();
                        }
                        return nullptr;
                }
#endif

                // ---- Bindings

                Picture *picture(asIScriptGeneric *gen) {
                        return static_cast<Picture *>(gen->GetObject());
                }
                Window *window(asIScriptGeneric *gen) {
                        Window *w = static_cast<Window *>(gen->GetObject());
                        w->checkClose();
                        return w;
                }
                int argInt(asIScriptGeneric *gen, asUINT n) {
                        return int(gen->GetArgDWord(n));
                }
                uint32_t argColor(asIScriptGeneric *gen, asUINT n) {
                        return gen->GetArgDWord(n) & 0xffffffu;
                }

                // Drawing on a picture (Picture::begin and end around it); @p covered, if given, is
                // painted whole by it.
                class Drawing {
                        public:
                                explicit Drawing(asIScriptGeneric *gen, const bmds_rect_t *covered = nullptr)
                                    : _p(picture(gen)) {
                                        _p->checkClose();
                                        _c = _p->begin(covered);
                                }
                                ~Drawing() { _p->end(); }
                                Picture       *owner() { return _p; }
                                bmds_canvas_t *canvas() { return _c; }
                                uint32_t       pixel(uint32_t rgb) { return bmds_canvas_pixel(_c, rgb); }

                        private:
                                Picture       *_p;
                                bmds_canvas_t *_c;
                };

                void width(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(picture(gen)->canvas.width));
                }
                void height(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(picture(gen)->canvas.height));
                }
                void clear(asIScriptGeneric *gen) {
                        const bmds_rect_t all = picture(gen)->canvas.clip;
                        Drawing           d(gen, &all);
                        d.owner()->fill(d.canvas(), all, d.pixel(argColor(gen, 0)));
                }
                void plot(asIScriptGeneric *gen) {
                        Drawing d(gen);
                        bmds_canvas_plot(d.canvas(), argInt(gen, 0), argInt(gen, 1), d.pixel(argColor(gen, 2)));
                }
                void pixelAt(asIScriptGeneric *gen) {
                        Drawing        d(gen);
                        const uint32_t v = bmds_canvas_rgb(d.canvas(),
                                                           bmds_canvas_get(d.canvas(), argInt(gen, 0), argInt(gen, 1)));
                        gen->SetReturnDWord(v);
                }
                void line(asIScriptGeneric *gen) {
                        Drawing d(gen);
                        bmds_canvas_line(d.canvas(), argInt(gen, 0), argInt(gen, 1), argInt(gen, 2), argInt(gen, 3),
                                         d.pixel(argColor(gen, 4)));
                }
                void rect(asIScriptGeneric *gen) {
                        Drawing d(gen);
                        bmds_canvas_rect(d.canvas(), argInt(gen, 0), argInt(gen, 1), argInt(gen, 2), argInt(gen, 3),
                                         d.pixel(argColor(gen, 4)));
                }
                void fillRect(asIScriptGeneric *gen) {
                        const int w = argInt(gen, 2), h = argInt(gen, 3);
                        if (w <= 0 || h <= 0) return;
                        const bmds_rect_t r = {argInt(gen, 0), argInt(gen, 1), uint32_t(w), uint32_t(h)};
                        Drawing           d(gen, &r);
                        d.owner()->fill(d.canvas(), r, d.pixel(argColor(gen, 4)));
                }
                void fillRectOpacity(asIScriptGeneric *gen) {
                        Drawing     d(gen);
                        const float opacity = gen->GetArgFloat(5);
                        const float o = opacity < 0 ? 0 : opacity > 1 ? 1 : opacity;
                        bmds_canvas_fill_blend(d.canvas(), argInt(gen, 0), argInt(gen, 1), argInt(gen, 2),
                                               argInt(gen, 3), argColor(gen, 4),
                                               uint32_t(o * float(BMDS_GAME_OPAQUE) + 0.5f));
                }
                void circle(asIScriptGeneric *gen) {
                        Drawing   d(gen);
                        const int r = argInt(gen, 2);
                        bmds_canvas_ellipse(d.canvas(), argInt(gen, 0), argInt(gen, 1), r, r,
                                            d.pixel(argColor(gen, 3)));
                }
                void fillCircle(asIScriptGeneric *gen) {
                        Drawing   d(gen);
                        const int r = argInt(gen, 2);
                        bmds_canvas_fill_ellipse(d.canvas(), argInt(gen, 0), argInt(gen, 1), r, r,
                                                 d.pixel(argColor(gen, 3)));
                }
                void ellipse(asIScriptGeneric *gen) {
                        Drawing d(gen);
                        bmds_canvas_ellipse(d.canvas(), argInt(gen, 0), argInt(gen, 1), argInt(gen, 2), argInt(gen, 3),
                                            d.pixel(argColor(gen, 4)));
                }
                void fillEllipse(asIScriptGeneric *gen) {
                        Drawing d(gen);
                        bmds_canvas_fill_ellipse(d.canvas(), argInt(gen, 0), argInt(gen, 1), argInt(gen, 2),
                                                 argInt(gen, 3), d.pixel(argColor(gen, 4)));
                }
                // The corners of array<int> (x0, y0, x1, y1...), or nullptr with an exception.
                const int32_t *corners(asIScriptGeneric *gen, uint32_t &count) {
                        const CScriptArray *a = static_cast<const CScriptArray *>(gen->GetArgAddress(0));
                        if (a->GetSize() % 2 != 0) {
                                raise("A polygon's points come in pairs (x, y), not %u numbers", a->GetSize());
                                return nullptr;
                        }
                        count = a->GetSize() / 2;
                        return count == 0 ? nullptr : static_cast<const int32_t *>(a->At(0));
                }
                void polygon(asIScriptGeneric *gen) {
                        uint32_t       count = 0;
                        const int32_t *xy = corners(gen, count);
                        if (xy == nullptr) return;
                        Drawing        d(gen);
                        const uint32_t pixel = d.pixel(argColor(gen, 1));
                        for (uint32_t i = 0; i < count; ++i) {
                                const uint32_t j = i + 1 == count ? 0 : i + 1;
                                bmds_canvas_line(d.canvas(), xy[2 * i], xy[2 * i + 1], xy[2 * j], xy[2 * j + 1], pixel);
                        }
                }
                void fillPolygon(asIScriptGeneric *gen) {
                        uint32_t       count = 0;
                        const int32_t *xy = corners(gen, count);
                        if (xy == nullptr) return;
                        Drawing d(gen);
                        if (bmds_canvas_fill_polygon(d.canvas(), xy, count, d.pixel(argColor(gen, 1))) != 0)
                                raise("Out of memory");
                }
                void floodFill(asIScriptGeneric *gen) {
                        Drawing d(gen);
                        if (bmds_canvas_flood_fill(d.canvas(), argInt(gen, 0), argInt(gen, 1),
                                                   d.pixel(argColor(gen, 2))) != 0)
                                raise("Out of memory");
                }
                int scaleOf(asIScriptGeneric *gen, asUINT n) {
                        const int s = argInt(gen, n);
                        return s < 1 ? 1 : s > 64 ? 64 : s;
                }
                void text(asIScriptGeneric *gen) {
                        Text t;
                        formatValue(gen->GetEngine(), t, gen->GetArgAddress(2), gen->GetArgTypeId(2));
                        Drawing d(gen);
                        gen->SetReturnDWord(
                                asDWORD(bmds_canvas_text(d.canvas(), argInt(gen, 0), argInt(gen, 1), t.data(),
                                                         d.pixel(argColor(gen, 3)), scaleOf(gen, 4))));
                }
                void textWidth(asIScriptGeneric *gen) {
                        Text t;
                        formatValue(gen->GetEngine(), t, gen->GetArgAddress(0), gen->GetArgTypeId(0));
                        gen->SetReturnDWord(asDWORD(bmds_game_text_width(t.data(), scaleOf(gen, 1))));
                }
                void textHeight(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(BMDS_GAME_GLYPH_HEIGHT * scaleOf(gen, 0)));
                }
                // An Image@ argument: AngelScript gives the function its reference, released here.
                class ImageArg {
                        public:
                                ImageArg(asIScriptGeneric *gen, asUINT n)
                                    : _i(static_cast<Image *>(gen->GetArgObject(n))) {
                                        if (_i == nullptr) raise("No image (null)");
                                }
                                ~ImageArg() {
                                        if (_i != nullptr) _i->release();
                                }
                                Image   *operator->() const { return _i; }
                                Image   &operator*() const { return *_i; }
                                explicit operator bool() const { return _i != nullptr; }

                        private:
                                Image *_i;
                };
                void drawImage(asIScriptGeneric *gen) {
                        ImageArg i(gen, 0);
                        if (!i) return;
                        Picture *p = picture(gen);
                        p->checkClose();
                        if (p->drawImageFast(*i, argInt(gen, 1), argInt(gen, 2))) return;
                        const bmds_game_image_t img = i->image();
                        Drawing                 d(gen);
                        bmds_canvas_blit(d.canvas(), argInt(gen, 1), argInt(gen, 2), &img, nullptr);
                }
                void drawImageScaled(asIScriptGeneric *gen) {
                        ImageArg i(gen, 0);
                        if (!i) return;
                        const bmds_game_image_t img = i->image();
                        Drawing                 d(gen);
                        bmds_canvas_blit_scaled(d.canvas(), argInt(gen, 1), argInt(gen, 2), argInt(gen, 3),
                                                argInt(gen, 4), &img, nullptr);
                }
                void drawImageRotated(asIScriptGeneric *gen) {
                        ImageArg i(gen, 0);
                        if (!i) return;
                        const bmds_game_image_t img = i->image();
                        float                   turns = gen->GetArgFloat(3) / 360.0f;
                        turns -= floorf(turns);
                        const float scale = gen->GetArgFloat(4);
                        if (scale <= 0) return;
                        Drawing d(gen);
                        bmds_canvas_blit_rotated(d.canvas(), argInt(gen, 1), argInt(gen, 2),
                                                 uint32_t(turns * float(BMDS_GAME_TURN)) % BMDS_GAME_TURN,
                                                 uint32_t(scale * float(BMDS_GAME_SCALE) + 0.5f), &img, nullptr);
                }
                void clip(asIScriptGeneric *gen) {
                        const bmds_rect_t r = {argInt(gen, 0), argInt(gen, 1),
                                               uint32_t(argInt(gen, 2) < 0 ? 0 : argInt(gen, 2)),
                                               uint32_t(argInt(gen, 3) < 0 ? 0 : argInt(gen, 3))};
                        picture(gen)->setClip(&r);
                }
                void noClip(asIScriptGeneric *gen) {
                        picture(gen)->setClip(nullptr);
                }
                void save(asIScriptGeneric *gen) {
                        const String *path = static_cast<const String *>(gen->GetArgAddress(0));
#ifdef ANGEL_BMDS
                        const int fd = ::open(path->cStr(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
                        if (fd < 0) {
                                raise("Can't write '%s': %s", path->cStr(), strerror(errno));
                                return;
                        }
                        int32_t r;
                        {
                                Drawing             d(gen);
                                bmds_image_encode_t e{};
                                e.size = sizeof(e);
                                e.codec = BMDS_IMAGE_PNG;
                                e.format = BMDS_PIXEL_BGR24;
                                e.width = uint32_t(d.canvas()->width);
                                e.height = uint32_t(d.canvas()->height);
                                e.stride = d.canvas()->stride;
                                e.pixels = d.canvas()->pixels;
                                r = bmds_image_encode(fd, &e);
                        }
                        close(fd);
                        if (r < 0) raise("Can't save '%s': %s", path->cStr(), strerror(-r));
#else
                        raise("Saving pictures (%s) needs BMDS", path->cStr());
#endif
                }

                // ---- Image's own

                Image *checkedImage(int width, int height) {
                        if (width < 1 || height < 1 || width > MaxSize || height > MaxSize) {
                                raise("An image is 1 to %d pixels each way, not %d x %d", MaxSize, width, height);
                                return nullptr;
                        }
                        Image *i = Image::make(width, height);
                        if (i == nullptr) raise("Out of memory for a %d x %d image", width, height);
                        return i;
                }
                void imageBlank(asIScriptGeneric *gen) {
                        gen->SetReturnAddress(checkedImage(argInt(gen, 0), argInt(gen, 1)));
                }
                void imageLoad(asIScriptGeneric *gen) {
                        const String *path = static_cast<const String *>(gen->GetArgAddress(0));
#ifdef ANGEL_BMDS
                        int    error = 0;
                        Image *i = Image::load(path->cStr(), error);
                        if (i == nullptr) {
                                raise("Can't load '%s': %s", path->cStr(), strerror(error < 0 ? -error : error));
                                return;
                        }
                        gen->SetReturnAddress(i);
#else
                        raise("Loading pictures (%s) needs BMDS", path->cStr());
#endif
                }
                void pictureAddRef(asIScriptGeneric *gen) {
                        picture(gen)->addRef();
                }
                void pictureRelease(asIScriptGeneric *gen) {
                        picture(gen)->release();
                }
                void imagePart(asIScriptGeneric *gen) {
                        Picture *p = picture(gen);
                        Image   *part = checkedImage(argInt(gen, 2), argInt(gen, 3));
                        if (part == nullptr) return;
                        bmds_game_image_t src = p->image();
                        src.flags = 0; // the pixels as they are, the key's too
                        const bmds_rect_t from = {argInt(gen, 0), argInt(gen, 1), uint32_t(argInt(gen, 2)),
                                                  uint32_t(argInt(gen, 3))};
                        bmds_canvas_blit(&part->canvas, 0, 0, &src, &from);
                        bmds_canvas_damage_clear(&part->canvas);
                        part->keyed = p->keyed;
                        part->key = p->key;
                        gen->SetReturnAddress(part);
                }
                void imageTransparent(asIScriptGeneric *gen) {
                        Picture *p = picture(gen);
                        p->keyed = true;
                        p->key = bmds_canvas_pixel(&p->canvas, argColor(gen, 0));
                }
                void imageOpaque(asIScriptGeneric *gen) {
                        picture(gen)->keyed = false;
                }
                // (Neither changes the pixels: a copy registered for the 2D-DMA stays good, and a
                // keyed image is never copied by it.)

                // ---- Window's own

                void windowOpen(asIScriptGeneric *gen) {
                        const String *title = static_cast<const String *>(gen->GetArgAddress(0));
                        gen->SetReturnAddress(
                                Window::open(title->cStr(), argInt(gen, 1), argInt(gen, 2), gen->GetArgByte(3) != 0));
                }
                void windowTitle(asIScriptGeneric *gen) {
                        window(gen)->setTitle(static_cast<const String *>(gen->GetArgAddress(0))->cStr());
                }
                void windowClosed(asIScriptGeneric *gen) {
                        gen->SetReturnByte(static_cast<Window *>(gen->GetObject())->closed());
                }
                void windowFrame(asIScriptGeneric *gen) {
                        window(gen)->frame();
                }
                void returnKey(asIScriptGeneric *gen, const RawEvent *e) {
                        const char *k = e == nullptr ? "" : e->text[0] != 0 ? e->text : e->key;
                        new (gen->GetAddressOfReturnLocation()) String(k);
                }
                void windowKey(asIScriptGeneric *gen) {
                        Window    *w = window(gen);
                        RawEvent   e;
                        const bool got = w->next(e, true, true);
                        w->checkClose();
                        returnKey(gen, got ? &e : nullptr);
                }
                void windowKeyNow(asIScriptGeneric *gen) {
                        RawEvent e;
                        returnKey(gen, window(gen)->next(e, false, true) ? &e : nullptr);
                }
                void windowKeyDown(asIScriptGeneric *gen) {
                        gen->SetReturnByte(
                                window(gen)->keyDown(static_cast<const String *>(gen->GetArgAddress(0))->cStr()));
                }
                void windowMouseX(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(window(gen)->mouseX()));
                }
                void windowMouseY(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(window(gen)->mouseY()));
                }
                void windowMouseDown(asIScriptGeneric *gen) {
                        gen->SetReturnByte(window(gen)->mouseDown(argInt(gen, 0)));
                }
                void windowMouseWheel(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(window(gen)->takeWheel()));
                }
                void windowEvent(asIScriptGeneric *gen, bool wait) {
                        Window *w = window(gen);
                        w->watchClose();
                        RawEvent e;
                        if (w->next(e, wait, false)) {
                                gen->SetReturnAddress(new Event(e));
                        } else if (wait && w->isClosed()) {
                                RawEvent closing{};
                                closing.type = Close;
                                gen->SetReturnAddress(new Event(closing));
                        } else {
                                gen->SetReturnAddress(nullptr);
                        }
                }
                void windowWaitEvent(asIScriptGeneric *gen) {
                        windowEvent(gen, true);
                }
                void windowNextEvent(asIScriptGeneric *gen) {
                        windowEvent(gen, false);
                }
                void windowPointer(asIScriptGeneric *gen) {
                        window(gen)->showPointer(gen->GetArgByte(0) != 0);
                }
                void eventAddRef(asIScriptGeneric *gen) {
                        static_cast<Event *>(gen->GetObject())->addRef();
                }
                void eventRelease(asIScriptGeneric *gen) {
                        static_cast<Event *>(gen->GetObject())->release();
                }

                // Calls @p f of @p object (with @p arg, an Event, if not null) nested in the running
                // script; an exception there is reported, and the script ends. @return What a bool
                // function returned; true for any other.
                bool callNested(asIScriptObject *object, asIScriptFunction *f, Event *arg) {
                        asIScriptContext *ctx = asGetActiveContext();
                        if (ctx == nullptr || ctx->PushState() < 0) return true;
                        int r = ctx->Prepare(f);
                        if (r >= 0) r = ctx->SetObject(object);
                        if (r >= 0 && arg != nullptr) r = ctx->SetArgObject(0, arg);
                        if (r >= 0) r = ctx->Execute();
                        if (r == asEXECUTION_EXCEPTION) {
                                fflush(stdout);
                                reportException(ctx);
                                restoreTerminal();
                                exit(1);
                        }
                        const bool result = r != asEXECUTION_FINISHED || f->GetReturnTypeId() != asTYPEID_BOOL ||
                                            ctx->GetReturnByte() != 0;
                        const asEContextState state = ctx->GetState();
                        ctx->PopState();
                        if (state == asEXECUTION_ABORTED) ctx->Abort();
                        return result;
                }

                void windowRun(asIScriptGeneric *gen) {
                        Window          *w = window(gen);
                        asIScriptObject *game = static_cast<asIScriptObject *>(gen->GetArgObject(0));
                        if (game == nullptr) {
                                raise("No game to run (null)");
                                return;
                        }
                        uint32_t hz = gen->GetArgDWord(1);
                        if (hz == 0 || hz > 1000) hz = 60;
                        asITypeInfo       *type = game->GetObjectType();
                        // tick() may say whether the picture changed (bool): false, and draw() waits.
                        asIScriptFunction *tick = type->GetMethodByDecl("void tick()");
                        if (tick == nullptr) tick = type->GetMethodByDecl("bool tick()");
                        asIScriptFunction *draw = type->GetMethodByDecl("void draw()");
                        asIScriptFunction *event = type->GetMethodByDecl("void event(Event@)");
                        // Closing the window ends run(); after it, as the script had it before.
                        const bool watched = w->closeWatched();
                        w->watchClose();
                        w->running = true;
                        const int64_t period = 1000000000LL / hz;
                        int64_t       due = monotonicNs();
                        // Drawn only when the picture may have changed: after a tick or an event (and
                        // at the start). Otherwise it sleeps until the next tick or event, rather than
                        // drawing the same picture at the display's rate: a game ticking ten times a
                        // second draws ten times a second.
                        bool changed = true;
                        while (w->running && !w->isClosed()) {
                                RawEvent e;
                                while (w->running && w->next(e, false, false)) {
                                        changed = true;
                                        if (event == nullptr) continue;
                                        Event *object = new Event(e);
                                        callNested(game, event, object);
                                        object->release();
                                }
                                const int64_t now = monotonicNs();
                                int           ran = 0;
                                while (w->running && now >= due && ran < MaxCatchUp) {
                                        if (tick == nullptr || callNested(game, tick, nullptr)) changed = true;
                                        due += period;
                                        ++ran;
                                }
                                if (now >= due) due = now + period; // too far behind: the rest dropped
                                if (!w->running) break;
                                if (!changed) {
                                        w->waitForEvent(due);
                                        w->checkClose();
                                        continue;
                                }
                                if (draw != nullptr) callNested(game, draw, nullptr);
                                changed = false;
                                w->frame(); // shown, and paced on the display
                        }
                        w->running = false;
                        if (!watched && !w->isClosed()) w->setCloseWatched(false);
                        game->Release(); // the argument's reference, the function's to release
                }
                void windowStop(asIScriptGeneric *gen) {
                        static_cast<Window *>(gen->GetObject())->running = false;
                }

                // ---- Colours

                struct NamedColor {
                                const char *name;
                                uint32_t    rgb;
                };
                constexpr NamedColor colors[] = {
                        {"Black", 0x000000},    {"White", 0xffffff},   {"Gray", 0x808080},   {"LightGray", 0xc0c0c0},
                        {"DarkGray", 0x404040}, {"Red", 0xff0000},     {"Green", 0x00a000},  {"Lime", 0x00ff00},
                        {"Blue", 0x0000ff},     {"Navy", 0x000080},    {"Sky", 0x87ceeb},    {"Yellow", 0xffff00},
                        {"Cyan", 0x00ffff},     {"Magenta", 0xff00ff}, {"Orange", 0xff8000}, {"Purple", 0x800080},
                        {"Brown", 0x8b4513},    {"Pink", 0xffc0cb},
                };
                uint32_t colorValues[sizeof(colors) / sizeof(colors[0])];

                uint32_t channel(int v) {
                        return uint32_t(v < 0 ? 0 : v > 255 ? 255 : v);
                }
                void rgb(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(channel(argInt(gen, 0)) << 16 | channel(argInt(gen, 1)) << 8 |
                                            channel(argInt(gen, 2)));
                }
                void hsv(asIScriptGeneric *gen) {
                        float       h = gen->GetArgFloat(0) / 60.0f;
                        const float s = fminf(fmaxf(gen->GetArgFloat(1), 0), 1),
                                    v = fminf(fmaxf(gen->GetArgFloat(2), 0), 1);
                        h -= 6.0f * floorf(h / 6.0f);
                        const int   sector = int(h) % 6;
                        const float f = h - floorf(h);
                        const float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
                        float       r = v, g = t, b = p;
                        switch (sector) {
                                case 1: r = q, g = v, b = p; break;
                                case 2: r = p, g = v, b = t; break;
                                case 3: r = p, g = q, b = v; break;
                                case 4: r = t, g = p, b = v; break;
                                case 5: r = v, g = p, b = q; break;
                                default: break;
                        }
                        gen->SetReturnDWord(uint32_t(r * 255 + 0.5f) << 16 | uint32_t(g * 255 + 0.5f) << 8 |
                                            uint32_t(b * 255 + 0.5f));
                }
                void mix(asIScriptGeneric *gen) {
                        const uint32_t a = argColor(gen, 0), b = argColor(gen, 1);
                        const float    t = fminf(fmaxf(gen->GetArgFloat(2), 0), 1);
                        uint32_t       out = 0;
                        for (int shift = 0; shift <= 16; shift += 8) {
                                const float x = float((a >> shift) & 0xff), y = float((b >> shift) & 0xff);
                                out |= uint32_t(x + (y - x) * t + 0.5f) << shift;
                        }
                        gen->SetReturnDWord(out);
                }

                // The methods both Window and Image have.
                void registerDrawing(asIScriptEngine *engine, const char *type) {
                        method(engine, type, "int width() const", width, "The width in pixels.");
                        method(engine, type, "int height() const", height, "The height in pixels.");
                        method(engine, type, "void clear(uint color = Color::Black)", clear,
                               "Fills it all with a colour (black).");
                        method(engine, type, "void plot(int x, int y, uint color)", plot,
                               "Sets one pixel (QBasic's PSET).");
                        method(engine, type, "uint pixel(int x, int y) const", pixelAt,
                               "The colour of one pixel, as drawn (QBasic's POINT); black outside.");
                        method(engine, type, "void line(int x1, int y1, int x2, int y2, uint color)", line,
                               "A line from one point to another, both ends included.");
                        method(engine, type, "void rect(int x, int y, int width, int height, uint color)", rect,
                               "A rectangle's outline, its top left at (x, y).");
                        method(engine, type, "void fillRect(int x, int y, int width, int height, uint color)", fillRect,
                               "A filled rectangle.");
                        method(engine, type,
                               "void fillRect(int x, int y, int width, int height, uint color, float opacity)",
                               fillRectOpacity, "A see-through rectangle: opacity 0 (none) to 1 (solid).");
                        method(engine, type, "void circle(int x, int y, int radius, uint color)", circle,
                               "A circle's outline around (x, y).");
                        method(engine, type, "void fillCircle(int x, int y, int radius, uint color)", fillCircle,
                               "A filled circle.");
                        method(engine, type, "void ellipse(int x, int y, int radiusX, int radiusY, uint color)",
                               ellipse, "An ellipse's outline around (x, y): its radii across and down.");
                        method(engine, type, "void fillEllipse(int x, int y, int radiusX, int radiusY, uint color)",
                               fillEllipse, "A filled ellipse.");
                        method(engine, type, "void polygon(const array<int> &in points, uint color)", polygon,
                               "A shape's outline through the points {x1, y1, x2, y2, ...}, closed.");
                        method(engine, type, "void fillPolygon(const array<int> &in points, uint color)", fillPolygon,
                               "A filled shape through the points {x1, y1, x2, y2, ...}.");
                        method(engine, type, "void floodFill(int x, int y, uint color)", floodFill,
                               "Fills the area of one colour around (x, y) with another (QBasic's PAINT).");
                        method(engine, type, "int text(int x, int y, const ?&in value, uint color, int scale = 1)",
                               text,
                               "Writes a value (text, a number...) with its top left at (x, y), each letter 8 x 16 "
                               "pixels times "
                               "scale; its width.");
                        method(engine, type, "void image(Image@ image, int x, int y)", drawImage,
                               "Draws an image with its top left at (x, y), leaving out its transparent pixels.");
                        method(engine, type, "void image(Image@ image, int x, int y, int width, int height)",
                               drawImageScaled, "Draws an image stretched to a size (negative: mirrored).");
                        method(engine, type,
                               "void imageRotated(Image@ image, int x, int y, float degrees, float scale = 1)",
                               drawImageRotated, "Draws an image turned clockwise and scaled, centred on (x, y).");
                        method(engine, type, "void clip(int x, int y, int width, int height)", clip,
                               "Drawing changes only this rectangle from now on.");
                        method(engine, type, "void clip()", noClip, "Drawing may change all of it again.");
                        method(engine, type, "void save(const string &in path)", save,
                               "Writes the picture to a PNG file.");
                        return;
                }

        } // namespace

        void waitForWindows() {
                for (Window *w : openWindows) {
                        if (w == nullptr || w->isClosed()) continue;
                        w->watchClose();
                        w->waitUntilClosed();
                }
                return;
        }

        void imageAddRef(void *image) {
                static_cast<Image *>(image)->addRef();
                return;
        }
        void imageRelease(void *image) {
                static_cast<Image *>(image)->release();
                return;
        }
        ImageInfo imageInfo(void *image) {
                const Image *i = static_cast<Image *>(image);
                return ImageInfo{i->canvas.width, i->canvas.height, i->generation};
        }
        void imageArgb(void *image, uint32_t *out) {
                const Image *i = static_cast<Image *>(image);
                for (int y = 0; y < i->canvas.height; ++y) {
                        const uint8_t *p = i->canvas.pixels + size_t(y) * i->canvas.stride;
                        for (int x = 0; x < i->canvas.width; ++x, p += Bpp) {
                                const uint32_t rgb = p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; // BGR24
                                *out++ = i->keyed && rgb == i->key ? 0u : 0xff000000u | rgb;
                        }
                }
                return;
        }

        void registerGraphics(asIScriptEngine *engine) {
                check(engine->SetDefaultNamespace("Color"), "namespace Color");
                for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
                        colorValues[i] = colors[i].rgb;
                        char decl[48];
                        snprintf(decl, sizeof(decl), "const uint %s", colors[i].name);
                        property(engine, decl, &colorValues[i], nullptr);
                }
                check(engine->SetDefaultNamespace(""), "namespace");
                def(engine, "uint rgb(int red, int green, int blue)", rgb,
                    "A colour from its red, green and blue, each 0 to 255.");
                def(engine, "uint hsv(float hue, float saturation, float value)", hsv,
                    "A colour from its hue (0 to 360 degrees: red, yellow, green, cyan, blue, magenta), saturation and "
                    "value "
                    "(0 to 1).");
                def(engine, "uint mix(uint color1, uint color2, float amount)", mix,
                    "A colour between two: amount 0 is the first, 1 the second.");
                def(engine, "int textWidth(const ?&in value, int scale = 1)", textWidth,
                    "How wide text() writes a value, in pixels.");
                def(engine, "int textHeight(int scale = 1)", textHeight, "How tall text() writes a line, in pixels.");

                check(engine->RegisterEnum("EventType"), "EventType");
                static constexpr const char *types[] = {"KeyDown", "KeyUp", "MouseMove", "MouseDown",
                                                        "MouseUp", "Wheel", "Close"};
                for (int i = 0; i < 7; ++i) check(engine->RegisterEnumValue("EventType", types[i], i), types[i]);
                check(engine->RegisterObjectType("Event", 0, asOBJ_REF), "Event");
                behaviour(engine, "Event", asBEHAVE_ADDREF, "void f()", eventAddRef, nullptr);
                behaviour(engine, "Event", asBEHAVE_RELEASE, "void f()", eventRelease, nullptr);
                check(engine->RegisterObjectProperty("Event", "const EventType type", asOFFSET(Event, type)),
                      "Event::type");
                check(engine->RegisterObjectProperty("Event", "const string key", asOFFSET(Event, key)), "Event::key");
                check(engine->RegisterObjectProperty("Event", "const string text", asOFFSET(Event, text)),
                      "Event::text");
                check(engine->RegisterObjectProperty("Event", "const int x", asOFFSET(Event, x)), "Event::x");
                check(engine->RegisterObjectProperty("Event", "const int y", asOFFSET(Event, y)), "Event::y");
                check(engine->RegisterObjectProperty("Event", "const int button", asOFFSET(Event, button)),
                      "Event::button");
                check(engine->RegisterObjectProperty("Event", "const int wheel", asOFFSET(Event, wheel)),
                      "Event::wheel");
                check(engine->RegisterObjectProperty("Event", "const bool repeat", asOFFSET(Event, repeat)),
                      "Event::repeat");

                check(engine->RegisterObjectType("Image", 0, asOBJ_REF), "Image");
                check(engine->RegisterObjectType("Screen", 0, asOBJ_REF), "Screen");
                check(engine->RegisterInterface("Game"), "Game");
                behaviour(engine, "Image", asBEHAVE_FACTORY, "Image@ f(int width, int height)", imageBlank,
                          "A blank (black) image to draw on.");
                behaviour(engine, "Image", asBEHAVE_FACTORY, "Image@ f(const string &in path)", imageLoad,
                          "A picture from a file (PNG, JPEG or BMP); transparent where the file is.");
                behaviour(engine, "Image", asBEHAVE_ADDREF, "void f()", pictureAddRef, nullptr);
                behaviour(engine, "Image", asBEHAVE_RELEASE, "void f()", pictureRelease, nullptr);
                registerDrawing(engine, "Image");
                method(engine, "Image", "Image@ part(int x, int y, int width, int height) const", imagePart,
                       "A new image of a rectangle of this one (a sprite from a sheet).");
                method(engine, "Image", "void transparent(uint color)", imageTransparent,
                       "Pixels of this colour aren't drawn when the image is.");
                method(engine, "Image", "void opaque()", imageOpaque, "Every pixel is drawn again.");

                behaviour(engine, "Screen", asBEHAVE_FACTORY,
                          "Screen@ f(const string &in title = \"Angel\", int width = 640, int height = 480, bool "
                          "fullScreen = false)",
                          windowOpen, "Opens a screen: a window to draw on (up to 1280 x 720); full screen if asked.");
                behaviour(engine, "Screen", asBEHAVE_ADDREF, "void f()", pictureAddRef, nullptr);
                behaviour(engine, "Screen", asBEHAVE_RELEASE, "void f()", pictureRelease, nullptr);
                registerDrawing(engine, "Screen");
                method(engine, "Screen", "void title(const string &in)", windowTitle, "Changes the window's title.");
                method(engine, "Screen", "bool closed()", windowClosed,
                       "The user closed the window. (A script that never asks ends when its window is closed.)");
                method(engine, "Screen", "void frame()", windowFrame,
                       "Shows what was drawn and waits for the screen's next frame: once round an animation's loop.");
                method(engine, "Screen", "string key()", windowKey,
                       "Waits for a key: a character (\"a\", \"A\", \" \"), or a name (\"Enter\", \"Up\", \"F1\"...).");
                method(engine, "Screen", "string keyNow()", windowKeyNow,
                       "A key pressed (as key()), or \"\" without waiting.");
                method(engine, "Screen", "bool keyDown(const string &in key)", windowKeyDown,
                       "The key is held down now: \"a\", \"Space\", \"Left\", \"Shift\"...");
                method(engine, "Screen", "int mouseX()", windowMouseX, "Where the mouse is across the window.");
                method(engine, "Screen", "int mouseY()", windowMouseY, "Where the mouse is down the window.");
                method(engine, "Screen", "bool mouseDown(int button = 1)", windowMouseDown,
                       "A mouse button is held down: 1 left, 2 right, 3 middle.");
                method(engine, "Screen", "int mouseWheel()", windowMouseWheel,
                       "The wheel's turns since last asked (positive: away from you).");
                method(engine, "Screen", "Event@ waitEvent()", windowWaitEvent,
                       "Waits for the next event (a key, the mouse, Close).");
                method(engine, "Screen", "Event@ nextEvent()", windowNextEvent,
                       "The next event, or null if none is waiting.");
                method(engine, "Screen", "void pointer(bool visible)", windowPointer,
                       "Shows or hides the mouse pointer over the window.");
                method(engine, "Screen", "void run(Game@ game, uint ticksPerSecond = 60)", windowRun,
                       "Runs a game until the window closes or stop(): its event(Event@) for each event, tick() so "
                       "many times a second, and draw() after them (each only if it has one): the picture is drawn "
                       "again only when a tick or an event may have changed it. A tick() declared bool returns "
                       "false when nothing changed.");
                method(engine, "Screen", "void stop()", windowStop, "Ends run().");
                return;
        }

} // namespace angel
