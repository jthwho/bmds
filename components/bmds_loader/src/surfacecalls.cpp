/**
 * @file      surfacecalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Surfaces' system calls (devplan/games.md §11): a program's picture in a window or full screen,
 * its buffers, presents, and the rest by operation.
 */

#include <cstring>
#include <bmds/errno.h>
#include <bmds/memory.h>
#include <bmds/process.h>
#include <bmds/programicons.h>
#include <bmds/programsurface.h>
#include <bmds/sys.h>
#include "running.h"
#include "structcopy.h"

using bmds::Error;
using bmds::KernelCall;
using bmds::ProgramSurface;
using bmds::Rect;
using bmds::SwapChain;

namespace {

        static_assert(sizeof(bmds_surface_event_t) == sizeof(ProgramSurface::Event) &&
                              offsetof(bmds_surface_event_t, time_ns) == offsetof(ProgramSurface::Event, timeNs) &&
                              offsetof(bmds_surface_event_t, data) == offsetof(ProgramSurface::Event, data),
                      "a surface's event records are the ABI's");
        static_assert(BMDS_PIXEL_L8 == uint32_t(ProgramSurface::Format::L8) &&
                              BMDS_PIXEL_RGB565 == uint32_t(ProgramSurface::Format::Rgb565) &&
                              BMDS_PIXEL_BGR24 == uint32_t(ProgramSurface::Format::Bgr24),
                      "the ABI's formats");
        static_assert(BMDS_SURFACE_FULL_SCREEN == ProgramSurface::FullScreen &&
                              BMDS_SURFACE_WHOLE == ProgramSurface::Whole &&
                              BMDS_SURFACE_FAST == ProgramSurface::Fast &&
                              BMDS_SURFACE_MAXIMIZED == ProgramSurface::Maximized,
                      "the ABI's flags");
        static_assert(BMDS_EVENT_KEY == uint32_t(ProgramSurface::EventType::Key) &&
                              BMDS_EVENT_POINTER == uint32_t(ProgramSurface::EventType::Pointer) &&
                              BMDS_EVENT_WHEEL == uint32_t(ProgramSurface::EventType::Wheel) &&
                              BMDS_EVENT_ACTIVE == uint32_t(ProgramSurface::EventType::Active) &&
                              BMDS_EVENT_PRESENTED == uint32_t(ProgramSurface::EventType::Presented) &&
                              BMDS_EVENT_CLOSE == uint32_t(ProgramSurface::EventType::Close) &&
                              BMDS_EVENT_ACTION == uint32_t(ProgramSurface::EventType::Action) &&
                              BMDS_EVENT_PAD == uint32_t(ProgramSurface::EventType::Pad),
                      "the ABI's events");
        static_assert(BMDS_KEY_DOWN == ProgramSurface::KeyDown && BMDS_KEY_UP == ProgramSurface::KeyUp &&
                              BMDS_KEY_REPEAT == ProgramSurface::KeyRepeat,
                      "the ABI's key flags");
        static_assert(BMDS_POINTER_NORMAL == uint32_t(ProgramSurface::PointerMode::Normal) &&
                              BMDS_POINTER_HIDDEN == uint32_t(ProgramSurface::PointerMode::Hidden) &&
                              BMDS_POINTER_RELATIVE == uint32_t(ProgramSurface::PointerMode::Relative),
                      "the ABI's pointer modes");
        static_assert(BMDS_POINTER_GONE == ProgramSurface::PointerGone, "the ABI's pointer gone");

        constexpr uint32_t MaxDamage = 64; // rectangles a present takes (more: all of it)
        constexpr size_t   MaxTitle = 128; // bytes of a title read

        // A program's surface, or a GUI canvas's (devplan/guiprograms.md §11 3c).
        int32_t surfaceOf(const bmds::FileHandle::Ptr &handle, SwapChain *&surface) {
                if (!handle) return -BMDS_EBADF;
                surface = handle->stream().swapChain();
                return surface != nullptr ? 0 : -BMDS_ENOTTY;
        }

        // io_poll's timeouts: negative forever, 0 only a check.
        unsigned int timeoutOf(int32_t ms) {
                return ms < 0 ? bmds::WaitForever : static_cast<unsigned int>(ms);
        }

        // Its program's icons (gui.md §4j) on a new surface's window: a program without them, or
        // one whose file can't be read, keeps the window's own.
        void giveIcons(bmds::FileHandle &handle, const char *path) {
                ProgramSurface *surface = handle.stream().programSurface();
                if (surface == nullptr || path == nullptr || path[0] == '\0') return;
                const bmds::Vfs::Context  context;
                bmds::ProgramIcons::Image small, large;
                const bmds::StringView    file(path, std::strlen(path));
                const bmds::Error serr = bmds::ProgramIcons::read(file, context, 16, bmds::MemSpace::Psram, small);
                const bmds::Error lerr = bmds::ProgramIcons::read(file, context, 32, bmds::MemSpace::Psram, large);
                // Only the sizes asked for: the nearest may be bigger (setIcons() refuses that).
                const auto fits = [](const bmds::ProgramIcons::Image &i, uint16_t side) {
                        return i.png != nullptr && i.width <= side && i.height <= side;
                };
                (void)surface->setIcons(
                        fits(small, 16) && serr.isOk() ? bmds::ByteSpan(small.png, small.bytes) : bmds::ByteSpan(),
                        fits(large, 32) && lerr.isOk() ? bmds::ByteSpan(large.png, large.bytes) : bmds::ByteSpan());
                bmds::Memory::free(small.png);
                bmds::Memory::free(large.png);
                return;
        }

        bmds::StringView textOf(const char *text, size_t max) {
                if (text == nullptr) return bmds::StringView();
                size_t n = 0;
                while (n < max && text[n] != '\0') ++n;
                return bmds::StringView(text, n);
        }

} // namespace

extern "C" {

int32_t bmds_k_surface_open(const bmds_surface_config_t *config) {
        KernelCall            call;
        bmds_surface_config_t c;
        const int32_t         r = bmds::detail::copyIn(config, c);
        if (r < 0) return r;
        if (c.width > UINT16_MAX || c.height > UINT16_MAX ||
            (c.format != BMDS_PIXEL_L8 && c.format != BMDS_PIXEL_RGB565 && c.format != BMDS_PIXEL_BGR24) ||
            c.buffers < 2 || c.buffers > ProgramSurface::MaxBuffers ||
            (c.flags & ~(BMDS_SURFACE_FULL_SCREEN | BMDS_SURFACE_WHOLE | BMDS_SURFACE_FAST | BMDS_SURFACE_MAXIMIZED)) !=
                    0 ||
            c.aspect_x > UINT16_MAX || c.aspect_y > UINT16_MAX)
                return -BMDS_EINVAL;
        ProgramSurface::Config k;
        k.width = uint16_t(c.width);
        k.height = uint16_t(c.height);
        k.format = ProgramSurface::Format(c.format);
        k.buffers = uint8_t(c.buffers);
        k.flags = uint8_t(c.flags);
        k.aspectX = uint16_t(c.aspect_x);
        k.aspectY = uint16_t(c.aspect_y);
        k.title = textOf(c.title, MaxTitle);
        if (k.title.isEmpty()) {
                // The program's name: its command's first word, its last part.
                const char *command = call.thread().run->command;
                const char *start = command;
                const char *end = command;
                for (; *end != '\0' && *end != ' '; ++end)
                        if (*end == '/') start = end + 1;
                k.title = bmds::StringView(start, size_t(end - start));
        }
        auto [handle, err] = ProgramSurface::open(k);
        if (err == Error::NotSupported) return -BMDS_ENODEV;
        if (err.isError()) return bmds::Process::errnoFor(err);
        giveIcons(*handle, call.thread().run->path);
        // Its program's actions, if declared (else the declaration gives them, devplan/controls.md §7c).
        if (bmds::ProgramControls *controls = call->existingControls(); controls != nullptr)
                handle->stream().programSurface()->setControls(controls);
        return call->addHandle(std::move(handle), 0);
}

int32_t bmds_k_surface_buffer(int32_t fd, bmds_surface_buffer_t *info) {
        KernelCall      call;
        SwapChain      *surface = nullptr;
        int32_t         r = surfaceOf(call->handle(fd), surface);
        if (r < 0) return r;
        bmds_surface_buffer_t i;
        r = bmds::detail::copyIn(info, i);
        if (r < 0) return r;
        uint8_t *pixels = i.index < SwapChain::MaxBuffers ? surface->buffer(uint8_t(i.index)) : nullptr;
        if (pixels == nullptr) return -BMDS_EINVAL;
        i.pixels = pixels;
        i.stride = uint32_t(surface->stride());
        return bmds::detail::copyOut(i, info);
}

int32_t bmds_k_surface_acquire(int32_t fd, int32_t timeout_ms) {
        KernelCall      call;
        SwapChain      *surface = nullptr;
        const int32_t   r = surfaceOf(call->handle(fd), surface);
        if (r < 0) return r;
        auto [index, err] = timeout_ms == 0 ? surface->tryAcquire() : surface->acquire(timeoutOf(timeout_ms));
        if (err == Error::WouldBlock || err == Error::Timeout) return -BMDS_EAGAIN;
        if (err.isError()) return bmds::Process::errnoFor(err);
        return index;
}

int32_t bmds_k_surface_present(int32_t fd, const bmds_surface_present_t *present) {
        KernelCall      call;
        SwapChain      *surface = nullptr;
        int32_t         r = surfaceOf(call->handle(fd), surface);
        if (r < 0) return r;
        bmds_surface_present_t p;
        r = bmds::detail::copyIn(present, p);
        if (r < 0) return r;
        if (p.index >= SwapChain::MaxBuffers) return -BMDS_EINVAL;
        // The damage, cut to the surface (more rectangles than kept: all of it).
        Rect   damage[MaxDamage];
        size_t n = 0;
        if (p.damage_count > 0 && p.damage_count <= MaxDamage) {
                if (p.damage == nullptr) return -BMDS_EFAULT;
                for (uint32_t i = 0; i < p.damage_count; ++i) {
                        const bmds_rect_t &d = p.damage[i];
                        const int64_t      x0 = d.x < 0 ? 0 : d.x, y0 = d.y < 0 ? 0 : d.y;
                        int64_t            x1 = int64_t(d.x) + d.width, y1 = int64_t(d.y) + d.height;
                        if (x1 > surface->width()) x1 = surface->width();
                        if (y1 > surface->height()) y1 = surface->height();
                        if (x1 <= x0 || y1 <= y0) continue;
                        damage[n++] = Rect{uint16_t(x0), uint16_t(y0), uint16_t(x1 - x0), uint16_t(y1 - y0)};
                }
                if (n == 0) return -BMDS_EINVAL; // nothing of it on the surface
        }
        auto [frame, err] = surface->present(uint8_t(p.index), bmds::Span<const Rect>(damage, n));
        if (err.isError()) return err == Error::Invalid ? -BMDS_EINVAL : bmds::Process::errnoFor(err);
        return int32_t(frame & 0x7fffffffu);
}

int32_t bmds_k_surface_control(int32_t fd, uint32_t op, void *arg, uint32_t size) {
        KernelCall      call;
        SwapChain      *surface = nullptr;
        int32_t         r = surfaceOf(call->handle(fd), surface);
        if (r < 0) return r;
        if (arg == nullptr) return -BMDS_EFAULT;
        switch (op) {
                case BMDS_SURFACE_PALETTE: {
                        bmds_surface_palette_t p;
                        r = bmds::detail::copyIn(static_cast<const bmds_surface_palette_t *>(arg), p);
                        if (r < 0) return r;
                        if (p.first > 256 || p.count > 256 - p.first || (p.count > 0 && p.rgb == nullptr))
                                return -BMDS_EINVAL;
                        const Error err = surface->setPalette(p.first, bmds::Span<const uint32_t>(p.rgb, p.count));
                        return err == Error::NotSupported ? -BMDS_ENOTSUP : err.isOk() ? 0 : -BMDS_EINVAL;
                }
                case BMDS_SURFACE_TITLE: {
                        const char  *text = static_cast<const char *>(arg);
                        const size_t n = textOf(text, size < MaxTitle ? size : MaxTitle).size();
                        return surface->setTitle(bmds::StringView(text, n)).isOk() ? 0 : -BMDS_EINVAL;
                }
                case BMDS_SURFACE_POINTER: {
                        if (size < sizeof(uint32_t)) return -BMDS_EINVAL;
                        const uint32_t mode = *static_cast<const uint32_t *>(arg);
                        if (mode > BMDS_POINTER_RELATIVE) return -BMDS_EINVAL;
                        return surface->setPointerMode(SwapChain::PointerMode(mode)).isOk() ? 0 : -BMDS_EINVAL;
                }
                case BMDS_SURFACE_WAIT: {
                        bmds_surface_wait_t w;
                        r = bmds::detail::copyIn(static_cast<const bmds_surface_wait_t *>(arg), w);
                        if (r < 0) return r;
                        if (w.timeout_ms == 0) return int32_t(surface->info().frames - w.frame) >= 0 ? 0 : -BMDS_EAGAIN;
                        const Error err = surface->waitFrame(w.frame, timeoutOf(w.timeout_ms));
                        if (err == Error::Timeout) return -BMDS_EAGAIN;
                        return err.isOk() ? 0 : bmds::Process::errnoFor(err);
                }
                case BMDS_SURFACE_INFO: {
                        const SwapChain::Info      k = surface->info();
                        bmds_surface_info_t        i = {};
                        i.size = sizeof(i);
                        i.full_screen = k.fullScreen ? 1 : 0;
                        i.active = k.active ? 1 : 0;
                        i.shown = k.shown ? 1 : 0;
                        i.scale_x16 = k.scaleX16;
                        i.scale_y16 = k.scaleY16;
                        i.x = k.area.x;
                        i.y = k.area.y;
                        i.width = k.area.width;
                        i.height = k.area.height;
                        i.frames = k.frames;
                        i.presents = k.presents;
                        i.dropped = k.dropped;
                        i.picture_width = k.width;
                        i.picture_height = k.height;
                        return bmds::detail::copyOut(i, static_cast<bmds_surface_info_t *>(arg));
                }
                case BMDS_SURFACE_IMAGE: { // devplan/games.md §25
                        bmds_surface_image_t im;
                        r = bmds::detail::copyIn(static_cast<const bmds_surface_image_t *>(arg), im);
                        if (r < 0) return r;
                        if (im.slot >= BMDS_SURFACE_IMAGES) return -BMDS_EINVAL;
                        SwapChain::Image k;
                        if (im.pixels != nullptr) {
                                if (im.width == 0 || im.height == 0 || im.width > UINT16_MAX || im.height > UINT16_MAX)
                                        return -BMDS_EINVAL;
                                // The program's own memory: the 2D-DMA reads it from now on.
                                const uint64_t bytes = uint64_t(im.stride) * im.height;
                                if (bytes > UINT32_MAX || !call.owner()->owns(im.pixels, size_t(bytes)))
                                        return -BMDS_EINVAL;
                                k.pixels = static_cast<const uint8_t *>(im.pixels);
                                k.width = uint16_t(im.width);
                                k.height = uint16_t(im.height);
                                k.stride = im.stride;
                        }
                        return surface->setImage(uint8_t(im.slot), k).isOk() ? 0 : -BMDS_EINVAL;
                }
                case BMDS_SURFACE_BLIT: {
                        bmds_surface_blit_t bl;
                        r = bmds::detail::copyIn(static_cast<const bmds_surface_blit_t *>(arg), bl);
                        if (r < 0) return r;
                        if (bl.count == 0 || bl.count > BMDS_SURFACE_BLIT_MAX || bl.source > BMDS_BLIT_FILL ||
                            bl.buffer >= SwapChain::MaxBuffers)
                                return -BMDS_EINVAL;
                        if (bl.pieces == nullptr) return -BMDS_EFAULT;
                        static_assert(sizeof(bmds_surface_piece_t) == sizeof(SwapChain::Piece) &&
                                              offsetof(bmds_surface_piece_t, from_y) ==
                                                      offsetof(SwapChain::Piece, fromY),
                                      "a blit's pieces are the ABI's");
                        const Error err =
                                surface->blit(uint8_t(bl.buffer), SwapChain::BlitSource(bl.source), bl.from,
                                              bmds::Span<const SwapChain::Piece>(
                                                      reinterpret_cast<const SwapChain::Piece *>(bl.pieces), bl.count));
                        if (err == Error::Invalid) return -BMDS_EINVAL;
                        return err.isOk() ? 0 : bmds::Process::errnoFor(err);
                }
                default: return -BMDS_EINVAL;
        }
}

} // extern "C"
