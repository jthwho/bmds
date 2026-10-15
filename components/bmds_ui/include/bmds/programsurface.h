/**
 * @file      programsurface.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/edid.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/icon.h>
#include <bmds/mutex.h>
#include <bmds/rect.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>
#include <bmds/span.h>
#include <bmds/stream.h>
#include <atomic>
#include <bmds/stringview.h>
#include <bmds/task.h>
#include <bmds/waitlist.h>
#include <bmds/windowmanager.h>

BMDS_NAMESPACE_BEGIN

class Painter;
class Surface;

/**
 * @brief A program's surface (`surface_open`, `devplan/games.md` §3, §10c, §11): buffers the
 *        program draws into with the CPU, presented in a window of the window manager (scaled by
 *        whole multiples) or full screen (Alt+Enter between them); its events read from its
 *        descriptor.
 *
 * Full screen, the scan shows a surface of its own: 1280 pixels across and as many rows as the
 * picture needs, which the scan repeats to fill the screen's lines (`Scanout`'s line mode), so
 * only the picture's own rows are written. Each present's rows are scaled across by the CPU into
 * strips in internal RAM and copied in by the 2D-DMA (RGB565 widened on the way), on the
 * surface's presenter task, while the program draws its next frame.
 *
 * The buffers are a swap chain: the program takes a free one (`acquire()`), draws, and presents
 * it. The newest presented one is shown and kept (an uncovered window is redrawn from it); the
 * one it replaced is free again once nothing reads it. A program presenting faster than the
 * screen shows only the newest: it paces itself with `waitFrame()`.
 *
 * The window manager's callbacks queue events (keys, the pointer in the surface's own pixels,
 * the wheel, focus, presentation, close); `read()` gives them as 32-byte records, the ABI's
 * `bmds_surface_event_t`.
 *
 * @par Thread Safety
 * Thread-safe: the program's threads and the window manager's task.
 */
class ProgramSurface final : public Stream {
        public:
                /** @brief Pixel formats: the ABI's `BMDS_PIXEL_...` values. */
                enum class Format : uint8_t {
                        Bgr24 = 0,  ///< B, G, R.
                        Rgb565 = 3, ///< Little-endian, R in the top 5 bits.
                        L8 = 5,     ///< An index into the palette of 256.
                };

                /** @brief `Config::flags`: the ABI's `BMDS_SURFACE_...` values. */
                enum Flag : uint8_t {
                        FullScreen = 0x1, ///< Suggest full screen.
                        Whole = 0x2,      ///< Whole multiples only.
                        Fast = 0x4,       ///< Buffers in internal RAM.
                };

                /** @brief Event types: the ABI's `BMDS_EVENT_...` values. */
                enum class EventType : uint32_t {
                        Key = 1,
                        Pointer = 2,
                        Wheel = 3,
                        Active = 4,
                        Presented = 5,
                        Close = 6,
                        Action = 7, ///< Reserved (`devplan/controls.md`).
                        Pad = 8,    ///< A gamepad's (`devplan/controls.md` §7b); flags: `PadEvent::Type`.
                };

                /** @brief A key event's flags: the ABI's. */
                enum KeyFlag : uint32_t {
                        KeyDown = 0x1,
                        KeyUp = 0x2,
                        KeyRepeat = 0x4,
                };

                /** @brief An event record: the ABI's `bmds_surface_event_t`. */
                struct Event {
                                uint32_t type = 0;
                                uint32_t flags = 0;
                                int64_t  timeNs = 0;
                                uint32_t data[4] = {};
                };
                static_assert(sizeof(Event) == 32, "the ABI's record");

                /** @brief Pointer modes (`setPointerMode()`): the ABI's. */
                enum class PointerMode : uint8_t {
                        Normal = 0,
                        Hidden = 1,
                        Relative = 2,
                };

                /** @brief What to open. */
                struct Config {
                                uint16_t   width = 0; ///< A multiple of 4.
                                uint16_t   height = 0;
                                Format     format = Format::Bgr24;
                                uint8_t    buffers = 2; ///< 2 or 3.
                                uint8_t    flags = 0;   ///< `Flag`s.
                                uint16_t   aspectX = 0; ///< The picture's shape (0, 0: square pixels).
                                uint16_t   aspectY = 0;
                                StringView title; ///< The window's (copied).
                };

                /** @brief Its state (`info()`). */
                struct Info {
                                bool     fullScreen = false;
                                bool     active = false;
                                bool     shown = false; ///< Has a window (or the screen).
                                uint16_t scaleX16 = 16; ///< The scale across, in 16ths.
                                uint16_t scaleY16 = 16; ///< Down.
                                Rect     area;          ///< The picture on the screen.
                                uint32_t frames = 0;    ///< The display's frames so far.
                                uint32_t presents = 0;
                                uint32_t dropped = 0; ///< Presents replaced before shown.
                };

                /** @brief Buffers at most. */
                static constexpr uint8_t MaxBuffers = 3;

                /** @brief The largest whole scale a window draws (`Pixels::repeat16()`'s). */
                static constexpr uint16_t MaxScale = 8;

                /**
                 * @brief Opens a surface: its buffers (black, PSRAM or with `Fast` internal RAM)
                 *        and its window on display 0's window manager.
                 * @return Its handle (read: events), or `Invalid` (the config), `NoMem`,
                 *         `NotSupported` (no window manager running).
                 */
                static Result<FileHandle::Ptr> open(const Config &config);

                /** @brief Returns the configuration (title excepted). */
                const Config &config() const { return _config; }

                /** @brief Returns buffer @p index's first pixel, or null. */
                uint8_t *buffer(uint8_t index) const { return index < _config.buffers ? _buffers[index] : nullptr; }

                /** @brief Returns the bytes from a row to the next. */
                size_t stride() const { return _stride; }

                /**
                 * @brief Takes a free buffer for the program to draw.
                 * @return Its index, or `Timeout` (`WaitForever`: never), `Interrupted`.
                 * @blocking
                 */
                Result<uint8_t> acquire(unsigned int timeoutMs);

                /**
                 * @brief Takes a free buffer if there is one.
                 * @return Its index, or `WouldBlock`.
                 * @nonblocking
                 */
                Result<uint8_t> tryAcquire();

                /**
                 * @brief Shows buffer @p index (the program's, from `acquire()`), which differs
                 *        from the last present in @p damage only (empty: all of it; a palette
                 *        changed since: all of it). Only that is converted (full screen) or
                 *        redrawn (a window); the buffer is whole all the same (an uncovered window
                 *        is redrawn from it). Writes it back from the cache.
                 * @return The frame it will be shown in, or `Invalid` (not the program's).
                 * @nonblocking
                 */
                Result<uint32_t> present(uint8_t index, Span<const Rect> damage);

                /** @brief Sets @p count palette entries (0xRRGGBB) from @p first: from the next present. */
                Error setPalette(size_t first, Span<const uint32_t> rgb);

                /** @brief Sets the window's title. */
                Error setTitle(StringView title);

                /**
                 * @brief Gives the window icons, its program's (`gui.md` §4j): @p small (a PNG of
                 *        16 × 16 at most: the title bar, the panel's button) and @p large (32 × 32
                 *        at most: Alt+Tab), either empty; decoded and kept with it.
                 * @return `Ok`, or `Invalid` (not a PNG of that size), `NoMem`.
                 */
                Error setIcons(ByteSpan small, ByteSpan large);

                /**
                 * @brief Sets what the pointer does over it: the arrow, none, or none and held
                 *        still while the window is focused, motion coming as the events' deltas.
                 * @return `Ok`, `Invalid`, `InvalidState` (no window).
                 */
                Error setPointerMode(PointerMode mode);

                /**
                 * @brief Waits until the display has begun frame @p frame (`Info::frames`).
                 * @return `Ok`, `Timeout`, `Interrupted`.
                 * @blocking
                 */
                Error waitFrame(uint32_t frame, unsigned int timeoutMs);

                /** @brief Returns its state. */
                Info info();

                /** @brief Returns the display's frames so far (the scan's: every surface's). */
                static uint32_t frames();

                // Stream
                Result<size_t>  read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t>  write(ByteSpan data, unsigned int timeoutMs) override;
                uint8_t         readiness() override;
                WaitList       *readinessWaiters() override { return &_pollers; }
                ProgramSurface *programSurface() override { return this; }
                void            handleClosed(uint8_t mode) override;

        private:
                enum class State : uint8_t {
                        Free,
                        Program,
                        Latest
                };
                enum Ask : uint8_t {
                        None,
                        Enter,
                        Leave
                };

                // Rectangles of the picture that changed, across on 8-pixel columns (PIE's 16 bytes in
                // RGB565): merged where they meet if that adds little; more than kept, the pair whose
                // bounds add the least merged.
                struct Damage {
                                static constexpr size_t Max = 16;
                                Rect                    rects[Max];
                                uint8_t                 count = 0;

                                void             add(const Rect &rect);
                                Span<const Rect> span() const { return Span<const Rect>(rects, count); }
                };

                // Full screen: the scan surface's rows, the black rows above the picture, where it
                // is across, its scale across (a whole factor; 0: width ÷ the picture's, any ratio,
                // games.md §22) and its width on the screen; the screen's mode (the desktop's, or one
                // of the monitor's the picture fills: games.md §20).
                struct Layout {
                                uint16_t rows = 0;
                                uint16_t top = 0;
                                uint16_t x = 0;
                                uint16_t scaleX = 1;
                                uint16_t width = 0;
                                uint16_t screenWidth = 0;
                                uint16_t screenHeight = 0;
                };

                ProgramSurface() = default;
                ~ProgramSurface();

                Error  start(const Config &config);
                void   closeWindow();
                void   push(const Event &event); // the queue's lock taken inside
                void   pushPresented();          // the window manager's lock held: no calls into it
                Rect   pictureRect(uint16_t clientWidth, uint16_t clientHeight, uint16_t &scale) const;
                void   draw(Painter &painter);
                void   drawScaled(Painter &painter, const uint8_t *buf, const Rect &pic, uint16_t scale,
                                  const Rect &clip);
                struct Band { // drawScaled()'s, for makeBand()
                                const ProgramSurface *self;
                                const uint8_t        *src; // the band's first row
                                uint16_t              scale;
                                uint16_t              rows; // source rows
                                uint16_t              c0;   // the first source column
                                uint16_t              cols;
                };
                static uint16_t makeBand(void *arg, uint8_t *strip, size_t stride, uint16_t column, uint16_t room);
                Damage damageOf(Span<const Rect> damage, bool all) const;
                void   expandRow(const uint8_t *src, uint16_t count, uint8_t *out, uint16_t scale) const;
                Layout layOut(uint16_t screenWidth, uint16_t screenHeight, bool own) const;
                void   ask(Ask what);
                void   run();
                Error  enterFullScreen();
                Error  enterFullScreen(const VideoTiming &mode, bool own);
                void   leaveFullScreen();
                void   convert();             // the newest present into the scan surface
                void   release(int8_t index); // done reading a presented buffer

                static void redrawEntry(void *arg, Painter &painter);
                static void keyEntry(void *arg, const KeyEvent &event);
                static void padEntry(void *arg, const PadEvent &event);
                void        padsSettled(bool active); // what is down on the pads, told or let go
                static void mouseEntry(void *arg, const WindowManager::MouseEvent &event);
                static void wheelEntry(void *arg, int notches);
                static void activatedEntry(void *arg, bool active);
                static void resizedEntry(void *arg, uint16_t width, uint16_t height);
                static void closedEntry(void *arg);
                static void goneEntry(void *arg);
                static void fullScreenEntry(void *arg, bool on);
                static void taskEntry(void *arg);

                static constexpr size_t QueueEvents = 64;

                Config         _config;
                WindowManager *_wm = nullptr;
                uint8_t        _client = 0;
                uint8_t        _window = 0; // 0: none
                uint8_t       *_buffers[MaxBuffers] = {};
                size_t         _stride = 0;
                size_t         _bufferBytes = 0;
                uint32_t      *_palette = nullptr; // 256 entries, 0xRRGGBB (L8)
                Icon           _icon;              // the window's (setIcons()): its pixels ours
                Icon           _largeIcon;
                bool           _paletteChanged = false; // since the last present (under _lock)
                uint8_t       *_strip = nullptr;        // full screen's rows, internal DMA RAM (while full screen)
                size_t         _stripBytes = 0;

                Mutex     _lock; // the buffers' states, the window's size
                State     _states[MaxBuffers] = {};
                int8_t    _latest = -1;  // the newest presented
                int8_t    _reading = -1; // being drawn by the window manager
                bool      _latestDrawn = false;
                Semaphore _freed{1, 0};
                uint16_t  _clientWidth = 0;
                uint16_t  _clientHeight = 0;
                bool      _active = false;
                uint32_t  _presents = 0;
                uint32_t  _dropped = 0;

                // The presenter task: switches, and full screen's conversion.
                Task                 _task;
                Semaphore            _work{8, 0};
                Semaphore            _taskDone{1, 0};
                std::atomic<uint8_t> _ask{None};
                std::atomic<bool>    _stopping{false};
                bool                 _taskStarted = false;
                Surface             *_fs = nullptr; // the scan's surface, while full screen (the task's)
                Layout               _layout;
                bool                 _fullScreen = false; // under _lock
                bool                 _dirty = false;      // a present to convert (full screen; under _lock)
                Damage               _pending;            // what to convert of it (under _lock)

                Mutex         _queueLock;
                Event         _queue[QueueEvents];
                size_t        _head = 0;
                size_t        _count = 0;
                Semaphore     _ready{1, 0};
                WaitListOf<4> _pollers;
};

BMDS_NAMESPACE_END
