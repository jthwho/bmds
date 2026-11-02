/**
 * @file      screenshot.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Display 0's picture as a PNG file (devplan/display.md §5d, devplan/notifications.md §4).
 */

#include <cstdio>
#include <cstring>
#include <esp_cache.h>
#include <bmds/display.h>
#include <bmds/jpegcodec.h>
#include <bmds/memory.h>
#include <bmds/screenshot.h>
#include <bmds/surface.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/texttemplate.h>
#include <bmds/windowmanager.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr unsigned int CaptureTimeoutMs = 200;
        constexpr unsigned int HoldSettleMs = 50;     // a frame's start, after holding the swaps
        constexpr size_t       SyncChunk = 64 * 1024; // a cache operation runs with interrupts off
        constexpr unsigned     MaxUnique = 99;        // `_2` to `_99`

        Error checkTemplate(StringView value) {
                return TextTemplate::check(value);
        }

        BMDS_SYSCONF(screenshotPath, Screenshot::PathKey, Config::Type::String,
                     "${HOME}/Images/Screenshots/${HOSTNAME}_${DATE}_${TIME}.png",
                     "Where Print Screen saves the screen: a template (${HOME}, ${HOSTNAME}, ${SERIAL}, "
                     "${DATE[:FORMAT]}, ${TIME[:FORMAT]})",
                     0, 0, nullptr, checkTemplate)

        struct Copy {
                        uint8_t       *to = nullptr;
                        size_t         bytes = 0; // its room
                        const Scanout *scan = nullptr;
                        uint16_t       width = 0; // what was taken
                        uint16_t       height = 0;
                        size_t         stride = 0;
        };

        void syncIn(const uint8_t *from, size_t bytes) {
                for (size_t off = 0; off < bytes; off += SyncChunk)
                        (void)esp_cache_msync(const_cast<uint8_t *>(from) + off,
                                              bytes - off < SyncChunk ? bytes - off : SyncChunk,
                                              ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                return;
        }

        // What the scan shows into the copy (the CPU never writes a shown buffer: no dirty lines to
        // lose): the desktop's @p surface, or a program's full screen in its place, at the mode
        // sent (the desktop's, or the monitor's own: games.md §20), its rows repeated as the scan
        // repeats them (line k shows row k × rows / lines). A full screen isn't freed meanwhile:
        // leaving it needs the window manager's lock, held while this runs.
        void copyShown(void *arg, const Surface &surface) {
                auto          *copy = static_cast<Copy *>(arg);
                const Surface *shown = copy->scan != nullptr ? copy->scan->surface() : nullptr;
                if (shown != nullptr && shown != &surface && shown->height() > 0) {
                        const uint32_t lines = copy->scan->timing().vActive;
                        const size_t   stride = shown->stride();
                        const uint32_t rows = shown->height();
                        if (rows <= lines && stride * lines <= copy->bytes) {
                                // The copy takes longer than a frame: the program's presents wait
                                // meanwhile (a swap begun already ends at the next frame's start),
                                // or its next frame lands in the buffer being copied.
                                auto *held = const_cast<Surface *>(shown);
                                held->holdSwaps(true);
                                (void)held->waitFrame(held->frames(), HoldSettleMs);
                                const uint8_t *from = shown->shown();
                                syncIn(from, stride * rows);
                                for (uint32_t k = 0; k < lines; ++k)
                                        std::memcpy(copy->to + size_t(k) * stride,
                                                    from + size_t(uint64_t(k) * rows / lines) * stride, stride);
                                held->holdSwaps(false);
                                copy->width = shown->width();
                                copy->height = uint16_t(lines);
                                copy->stride = stride;
                                return;
                        }
                }
                syncIn(surface.shown(), surface.stride() * surface.height());
                std::memcpy(copy->to, surface.shown(), surface.stride() * surface.height());
                copy->width = surface.width();
                copy->height = surface.height();
                copy->stride = surface.stride();
                return;
        }

        Error toFile(void *arg, ByteSpan data) {
                auto *file = static_cast<FileHandle *>(arg);
                while (!data.isEmpty()) {
                        auto [n, err] = file->write(data, WaitForever);
                        if (err.isError()) return err;
                        if (n == 0) return Error::IoError;
                        data = ByteSpan(data.data() + n, data.size() - n);
                }
                return Error::Ok;
        }

        // A name ending in .jpg or .jpeg (any case): written as a JPEG.
        bool isJpegName(StringView path) {
                const size_t dot = path.lastIndexOf('.');
                if (dot == StringView::NPos) return false;
                const StringView ext = path.mid(dot + 1);
                char             lower[5] = {};
                if (ext.size() > 4) return false;
                for (size_t i = 0; i < ext.size(); ++i) lower[i] = char(ext[i] | 0x20);
                return StringView(lower) == "jpg" || StringView(lower) == "jpeg";
        }

        struct Counted {
                        FileHandle *file;
                        size_t      bytes = 0;
        };

        Error countedToFile(void *arg, ByteSpan data) {
                auto *c = static_cast<Counted *>(arg);
                c->bytes += data.size();
                return toFile(c->file, data);
        }

        // Each directory above the file @p path (normalized) made.
        Error makeDirs(StringView path, const Vfs::Context &context) {
                auto [full, err] = Vfs::normalize(path, context.cwd);
                if (err.isError()) return err;
                const StringView p = full.view();
                for (size_t i = 1; i < p.size(); ++i) {
                        if (p[i] != '/') continue;
                        const Error made = Vfs::mkdir(p.left(i), context);
                        if (made.isError() && made != Error::Exists) return made;
                }
                return Error::Ok;
        }

        // @p path with `_N` before its extension (the last '.' after the last '/').
        Error numbered(StringView path, unsigned n, String &out) {
                size_t dot = path.size();
                for (size_t i = path.size(); i > 0; --i) {
                        if (path[i - 1] == '/') break;
                        if (path[i - 1] == '.') {
                                dot = i - 1;
                                break;
                        }
                }
                out.clear();
                Error err = out.append(path.left(dot));
                if (err.isOk()) err = out.appendf("_%u", n);
                if (err.isOk()) err = out.append(path.mid(dot));
                return err;
        }

} // namespace

Error Screenshot::configuredPath(String &out) {
        String      text(MemSpace::Psram);
        const Error err = SysConf::get(StringView(PathKey), text);
        if (err.isError()) return err;
        return SysConf::expand(text.view(), out);
}

Error Screenshot::save(String &path, int level, uint8_t flags, const Vfs::Context &context, Report &report) {
        report = Report{};
        auto [d, derr] = Display::display(0);
        if (derr.isOk() && !d->isRunning()) derr = Error::NotReady;
        if (derr.isError()) {
                report.failed = "display 0";
                return derr;
        }
        Surface       &surface = d->surface();
        const size_t   bytes = surface.stride() * surface.height();
        const uint64_t start = System::uptimeUs();
        Copy           copy;
        copy.to = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, bytes));
        copy.bytes = bytes;
        copy.scan = &d->scanout();
        if (copy.to == nullptr) {
                report.failed = "copy";
                return Error::NoMem;
        }
        // The desktop's last round whole (or a full screen's latest); without one, the surface as it is.
        Error err;
        auto [wm, werr] = WindowManager::manager(0);
        if (werr.isOk() && wm->isRunning()) {
                err = wm->capture(copyShown, &copy, CaptureTimeoutMs);
        } else {
                copyShown(&copy, surface);
        }
        report.copyUs = uint32_t(System::uptimeUs() - start);
        report.width = copy.width;
        report.height = copy.height;
        if (err.isError()) {
                Memory::free(copy.to);
                report.failed = "capture";
                return err;
        }
        if ((flags & MakeDirs) != 0) err = makeDirs(path.view(), context);
        FileHandle::Ptr file;
        if (err.isOk() && (flags & Unique) != 0) {
                String asked(MemSpace::Psram);
                err = asked.append(path.view());
                for (unsigned n = 1; err.isOk(); ++n) {
                        if (n > 1) err = n <= MaxUnique ? numbered(asked.view(), n, path) : Error::Exists;
                        if (err.isError()) break;
                        auto [f, ferr] = Vfs::open(path.view(), Vfs::Write | Vfs::Create | Vfs::Exclusive, context);
                        if (ferr == Error::Exists) continue;
                        err = ferr;
                        file = std::move(f);
                        break;
                }
        } else if (err.isOk()) {
                auto [f, ferr] = Vfs::open(path.view(), Vfs::Write | Vfs::Create | Vfs::Truncate, context);
                err = ferr;
                file = std::move(f);
        }
        if (err.isOk() && isJpegName(path.view())) {
                // A JPEG by the codec (devplan/images.md §7): the copy is the codec's B, G, R already.
                auto [codec, cerr] = JpegCodec::codec(0);
                err = cerr;
                if (err.isOk()) {
                        ImageBuffer in;
                        in.pixels = copy.to;
                        in.width = copy.width;
                        in.height = copy.height;
                        in.stride = copy.stride;
                        in.format = PixelFormat::Bgr24;
                        Counted counted{&*file};
                        err = codec->encode(in, JpegCodec::Encode{}, countedToFile, &counted, WaitForever);
                        report.stats.bytes = counted.bytes;
                        report.stats.compressUs = uint32_t(System::uptimeUs() - start) - report.copyUs;
                }
        } else if (err.isOk()) {
                err = Png::write(copy.to, copy.width, copy.height, copy.stride, level, toFile, &*file, &report.stats);
        }
        Memory::free(copy.to);
        file.reset(); // closed: written out
        report.totalUs = uint32_t(System::uptimeUs() - start);
        return err;
}

BMDS_NAMESPACE_END
