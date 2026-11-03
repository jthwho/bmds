/**
 * @file      bmdsplatform.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "bmdsplatform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bmds/sys.h>

namespace preso {

        namespace {

                // The presenter's actions (devplan/controls.md §7c): the defaults, which the user
                // may change in Controls. The Ids are Preso's Action numbers. Four keys an action at
                // most; Enter is left a key (Next, or the end of a slide's number: step 5).
                constexpr char Controls[] =
                        "[Controls]\n"
                        "Sets=deck;overview\n"
                        "[Action next]\n"
                        "Id=1\nLabel=Next\nGroup=Moving\nSets=deck\nKeys=Right;Down;Space;PageDown\n"
                        "Pad=Pad.South;Pad.Right;Pad.R1\nRepeat=true\n"
                        "[Action previous]\n"
                        "Id=2\nLabel=Previous\nGroup=Moving\nSets=deck\nKeys=Left;Up;Backspace;PageUp\n"
                        "Pad=Pad.East;Pad.Left;Pad.L1\nRepeat=true\n"
                        "[Action first]\n"
                        "Id=3\nLabel=First slide\nGroup=Moving\nSets=deck\nKeys=Home\n"
                        "[Action last]\n"
                        "Id=4\nLabel=Last slide\nGroup=Moving\nSets=deck\nKeys=End\n"
                        "[Action quit]\n"
                        "Id=5\nLabel=Quit\nGroup=Presenting\nSets=deck\nKeys=Escape\n"
                        "[Action black]\n"
                        "Id=6\nLabel=Black screen\nGroup=Presenting\nSets=deck\nKeys=B;Period\nPad=Pad.Select\n"
                        "[Action white]\n"
                        "Id=7\nLabel=White screen\nGroup=Presenting\nSets=deck\nKeys=W\n"
                        "[Action overview]\n"
                        "Id=8\nLabel=Every slide\nGroup=Presenting\nSets=deck\nKeys=Tab\nPad=Pad.Start\n"
                        "[Action left]\n"
                        "Id=9\nLabel=Left\nGroup=Overview\nSets=overview\nKeys=Left\nPad=Pad.Left;Pad.LeftX-\nRepeat="
                        "true\n"
                        "[Action right]\n"
                        "Id=10\nLabel=Right\nGroup=Overview\nSets=overview\nKeys=Right\nPad=Pad.Right;Pad.LeftX+\n"
                        "Repeat=true\n"
                        "[Action up]\n"
                        "Id=11\nLabel=Up\nGroup=Overview\nSets=overview\nKeys=Up\nPad=Pad.Up;Pad.LeftY-\nRepeat=true\n"
                        "[Action down]\n"
                        "Id=12\nLabel=Down\nGroup=Overview\nSets=overview\nKeys=Down\nPad=Pad.Down;Pad.LeftY+\nRepeat="
                        "true\n"
                        "[Action choose]\n"
                        "Id=13\nLabel=Go to it\nGroup=Overview\nSets=overview\nKeys=Enter;Space\nPad=Pad.South\n"
                        "[Action close]\n"
                        "Id=14\nLabel=Back\nGroup=Overview\nSets=overview\nKeys=Tab;Escape\nPad=Pad.East;Pad.Start\n";

                static_assert(uint32_t(Action::Next) == 1 && uint32_t(Action::Previous) == 2 &&
                                      uint32_t(Action::First) == 3 && uint32_t(Action::Last) == 4 &&
                                      uint32_t(Action::Quit) == 5 && uint32_t(Action::Black) == 6 &&
                                      uint32_t(Action::White) == 7 && uint32_t(Action::Overview) == 8 &&
                                      uint32_t(Action::Left) == 9 && uint32_t(Action::Close) == 14,
                              "Controls' Ids are the actions");

                // Without Controls (an older system): the same keys, by their HID usages.
                Action actionForKey(uint32_t usage, InputMode mode) {
                        usage &= 0xFFFF;
                        if (mode == InputMode::Overview) {
                                switch (usage) {
                                        case 0x50: return Action::Left;
                                        case 0x4F: return Action::Right;
                                        case 0x52: return Action::Up;
                                        case 0x51: return Action::Down;
                                        case 0x28:
                                        case 0x2C: return Action::Choose;
                                        case 0x2B:
                                        case 0x29: return Action::Close;
                                        default: return Action::None;
                                }
                        }
                        switch (usage) {
                                case 0x4F: // Right
                                case 0x51: // Down
                                case 0x2C: // Space
                                case 0x4E: return Action::Next;
                                case 0x50: // Left
                                case 0x52: // Up
                                case 0x2A: // Backspace
                                case 0x4B: return Action::Previous;
                                case 0x4A: return Action::First;
                                case 0x4D: return Action::Last;
                                case 0x29: return Action::Quit;
                                case 0x05: // B
                                case 0x37: return Action::Black;
                                case 0x1A: return Action::White;
                                case 0x2B: return Action::Overview;
                                default: return Action::None;
                        }
                }

        } // namespace

        BmdsPlatform::~BmdsPlatform() {
                if (_watch >= 0) bmds_close(_watch);
                if (_surface >= 0) bmds_close(_surface);
        }

        // ---- Fonts: the system's, by name.

        int32_t BmdsPlatform::openFont(const char *name) {
                return bmds_font_open_name(name);
        }

        void BmdsPlatform::closeFont(int32_t font) {
                (void)bmds_font_close(font);
                return;
        }

        FontMetrics BmdsPlatform::fontMetrics(int32_t font) {
                bmds_font_info_t info;
                memset(&info, 0, sizeof info);
                info.size = sizeof info;
                FontMetrics m;
                if (bmds_font_info(font, &info) == 0) {
                        m.ascent = info.ascent;
                        m.descent = info.descent;
                        m.lineHeight = info.line_height;
                }
                return m;
        }

        int32_t BmdsPlatform::textWidth(int32_t font, StringView text) {
                bmds_text_extent_t e = {sizeof e, 0, 0, 0};
                if (bmds_text_measure(font, text.data(), text.length(), 0, &e) != 0) return 0;
                return int32_t(e.width);
        }

        uint32_t BmdsPlatform::textFit(int32_t font, StringView text, int32_t maxWidth) {
                if (maxWidth <= 0) return 0;
                bmds_text_extent_t e = {sizeof e, 0, 0, 0};
                if (bmds_text_measure(font, text.data(), text.length(), uint32_t(maxWidth), &e) != 0) return 0;
                return e.bytes;
        }

        void BmdsPlatform::drawText(Canvas &canvas, int32_t font, int32_t x, int32_t y, StringView text, Color color) {
                if (text.isEmpty()) return;
                const Rect      &clip = canvas.clip();
                bmds_text_draw_t d;
                memset(&d, 0, sizeof d);
                d.size = sizeof d;
                d.font = font;
                d.pixels = canvas.pixels();
                d.format = canvas.format() == PixelFormat::Rgb565 ? BMDS_PIXEL_RGB565 : BMDS_PIXEL_BGRA32;
                d.width = uint32_t(canvas.width());
                d.height = uint32_t(canvas.height());
                d.stride = canvas.stride();
                d.clip_x = clip.x;
                d.clip_y = clip.y;
                d.clip_width = uint32_t(clip.width);
                d.clip_height = uint32_t(clip.height);
                d.x = x;
                d.y = y;
                d.color = color;
                d.text = text.data();
                d.length = text.length();
                (void)bmds_text_draw(&d);
                return;
        }

        // ---- Colouring code: the editor's languages.

        int32_t BmdsPlatform::findSyntax(StringView name) {
                int32_t language = bmds_syntax_find(name.data(), name.length());
                return language >= 0 ? language : -1;
        }

        uint32_t BmdsPlatform::colorLine(int32_t syntax, StringView line, uint32_t *state, SyntaxSpan *spans,
                                         uint32_t max) {
                constexpr uint32_t Max = 64;
                bmds_syntax_span_t got[Max];
                bmds_syntax_scan_t scan;
                memset(&scan, 0, sizeof scan);
                scan.size = sizeof scan;
                scan.language = syntax;
                scan.line = line.data();
                scan.length = line.length();
                scan.state = *state;
                scan.spans = got;
                scan.max_spans = max < Max ? max : Max;
                if (bmds_syntax_scan(&scan) != 0) return 0;
                *state = scan.state;
                for (uint32_t i = 0; i < scan.count; i++) {
                        spans[i].start = got[i].start;
                        spans[i].end = got[i].end;
                        spans[i].style = uint8_t(got[i].style);
                }
                return scan.count;
        }

        // ---- Pictures: the system's decoders (PNG, the JPEG codec or TJpgDec, BMP), upright.

        namespace {

                Error errorOf(int32_t r) {
                        switch (-r) {
                                case BMDS_ENOENT: return Error::NotFound;
                                case BMDS_ENOMEM: return Error::NoMemory;
                                case BMDS_ERANGE: return Error::TooBig; // past image/decode-max
                                case BMDS_ENOTSUP: return Error::Invalid;
                                default: return Error::Io;
                        }
                }

                // The picture's header asked about: what decoding it upright, as BGRA32, gives.
                int32_t ask(int32_t fd, const void *data, uint32_t length, bmds_image_decode_t &want,
                            bmds_image_info_t &info) {
                        memset(&info, 0, sizeof info);
                        info.size = sizeof info;
                        return bmds_image_info(fd, data, length, &want, &info);
                }

        } // namespace

        Error BmdsPlatform::imageSize(const char *path, int32_t *width, int32_t *height) {
                *width = *height = 0;
                int32_t fd = bmds_open(path, BMDS_O_READ | BMDS_O_CLOEXEC);
                if (fd < 0) return errorOf(fd);
                bmds_image_decode_t want;
                memset(&want, 0, sizeof want);
                want.size = sizeof want;
                want.format = BMDS_PIXEL_BGRA32;
                want.flags = BMDS_IMAGE_ORIENT;
                bmds_image_info_t info;
                int32_t           r = ask(fd, nullptr, 0, want, info);
                bmds_close(fd);
                if (r < 0 && r != -BMDS_ERANGE) return errorOf(r);
                // Too big to decode whole is still a size (a JPEG may come reduced).
                *width = int32_t(info.decode_width != 0 ? info.decode_width : info.width);
                *height = int32_t(info.decode_height != 0 ? info.decode_height : info.height);
                return *width > 0 && *height > 0 ? Error::Ok : Error::Invalid;
        }

        Error BmdsPlatform::decodeImage(const char *path, int32_t minWidth, int32_t minHeight, Picture *picture) {
                *picture = Picture();
                char    *file = nullptr;
                uint32_t length = 0;
                Error    error = readFile(path, 64u << 20, &file, &length);
                if (error != Error::Ok) return error;
                bmds_image_decode_t d;
                memset(&d, 0, sizeof d);
                d.size = sizeof d;
                d.format = BMDS_PIXEL_BGRA32;
                d.flags = BMDS_IMAGE_ORIENT;
                // No smaller than drawn: a JPEG comes reduced by halves toward it, and decodes faster.
                d.min_width = uint32_t(minWidth > 0 ? minWidth : 0);
                d.min_height = uint32_t(minHeight > 0 ? minHeight : 0);
                d.data = file;
                d.length = length;
                bmds_image_info_t info;
                int32_t           r = ask(-1, file, length, d, info);
                // An opaque picture as bytes B, G, R, else (no memory for that) RGB565: the JPEG codec's
                // own formats, decoded straight into the buffer. As BGRA32 a JPEG decodes elsewhere
                // and converts, the picture's memory twice over: a 1920×1080 one ran out of memory so
                // (devplan/preso.md §14). RGB565 is two thirds of BGR24, but its five bits of red and
                // blue band a sky once scaled: the last resort.
                bool opaque = r == 0 && (info.flags & BMDS_IMAGE_ALPHA) == 0;
                for (int attempt = 0; opaque && attempt < 2; attempt++) {
                        d.format = attempt == 0 ? BMDS_PIXEL_BGR24 : BMDS_PIXEL_RGB565;
                        r = ask(-1, file, length, d, info);
                        if (r < 0) break;
                        uint32_t bpp = attempt == 0 ? 3 : 2;
                        d.out_size = info.decode_width * info.decode_height * bpp;
                        d.stride = info.decode_width * bpp;
                        uint32_t align = 64;
                        if (info.align != 0 && info.bytes != 0) {
                                d.out_size = info.bytes;
                                d.stride = info.stride;
                                if (info.align > align) align = info.align;
                        }
                        d.out = bmds_mem_alloc(d.out_size, align, 0);
                        if (d.out != nullptr) break;
                }
                if (r == 0 && (info.decode_width == 0 || info.decode_height == 0)) r = -BMDS_ENOTSUP;
                if (r < 0) {
                        free(file);
                        if (d.out != nullptr) bmds_mem_free(d.out);
                        return errorOf(r);
                }
                if (!opaque) {
                        // Laid out as the decoder asks (aligned, its stride), it decodes without a copy.
                        d.out_size = info.decode_width * info.decode_height * 4;
                        d.stride = info.decode_width * 4;
                        d.out = bmds_mem_alloc(d.out_size, 64, 0);
                }
                if (d.out == nullptr) {
                        free(file);
                        return Error::NoMemory;
                }
                r = bmds_image_decode(-1, &d, nullptr);
                free(file);
                if (r < 0) {
                        freePixels(d.out);
                        return errorOf(r);
                }
                picture->pixels = static_cast<uint32_t *>(d.out);
                picture->width = int32_t(info.decode_width);
                picture->height = int32_t(info.decode_height);
                picture->stride = d.stride;
                picture->layout = !opaque                        ? Picture::Layout::Bgra32
                                  : d.format == BMDS_PIXEL_BGR24 ? Picture::Layout::Bgr24
                                                                 : Picture::Layout::Rgb565;
                picture->alpha = (info.flags & BMDS_IMAGE_ALPHA) != 0;
                return Error::Ok;
        }

        // ---- Pixels: the system's allocator, so the 2D-DMA may copy from them.

        void *BmdsPlatform::allocatePixels(uint32_t bytes) {
                return bmds_mem_alloc(bytes, 64, 0);
        }

        void BmdsPlatform::freePixels(void *pixels) {
                bmds_mem_free(pixels);
                return;
        }

        // ---- The screen

        const char *BmdsPlatform::dataFolder() {
                // The packages' share folder: `pkg/root`'s (where dist/preso installs).
                if (_data[0] != '\0') return _data;
                char    root[200];
                int32_t id = bmds_sysconf_find("pkg/root");
                if (id < 0 || bmds_sysconf_get(id, BMDS_SYSCONF_FIELD_VALUE, root, sizeof root) < 0 ||
                    root[0] == '\0') {
                        return nullptr;
                }
                snprintf(_data, sizeof _data, "%s/share/preso", root);
                return _data;
        }

        Error BmdsPlatform::openScreen(int32_t width, int32_t height, const char *title, bool sharp) {
                bmds_surface_config_t c;
                memset(&c, 0, sizeof c);
                c.size = sizeof c;
                c.width = uint32_t(width);
                c.height = uint32_t(height);
                c.format = BMDS_PIXEL_RGB565;
                c.buffers = 2;
                // Sharp: whole multiples only, so a 640×360 theme's pixels stay square and crisp.
                c.flags = BMDS_SURFACE_FULL_SCREEN | (sharp ? BMDS_SURFACE_WHOLE : 0u);
                c.title = title;
                _surface = bmds_surface_open(&c);
                if (_surface < 0) {
                        fprintf(stderr, "preso: no screen: %s\n", strerror(-_surface));
                        return _surface == -BMDS_ENOMEM ? Error::NoMemory : Error::NotSupported;
                }
                _width = width;
                _height = height;
                _bufferCount = 2;
                for (uint32_t i = 0; i < _bufferCount; i++) {
                        bmds_surface_buffer_t b;
                        memset(&b, 0, sizeof b);
                        b.size = sizeof b;
                        b.index = i;
                        if (bmds_surface_buffer(_surface, &b) != 0) return Error::Io;
                        _buffers[i] = b.pixels;
                        _strides[i] = b.stride;
                }
                // The presenter's actions; without them (an older system), keys by their usages.
                int32_t r = bmds_controls(BMDS_CONTROLS_DECLARE, const_cast<char *>(Controls), sizeof Controls - 1);
                if (r == 0) r = bmds_controls(BMDS_CONTROLS_SET, const_cast<char *>("deck"), 4);
                _controls = r == 0;
                if (!_controls && r != -BMDS_ENOSYS) fprintf(stderr, "preso: its controls: %s\n", strerror(-r));
                // No arrow over the slides (the presenter's pointer is Preso's own: step 8).
                uint32_t pointer = BMDS_POINTER_HIDDEN;
                (void)bmds_surface_control(_surface, BMDS_SURFACE_POINTER, &pointer, sizeof pointer);
                return Error::Ok;
        }

        void BmdsPlatform::present(const Canvas &canvas) {
                if (_surface < 0) return;
                int32_t index = bmds_surface_acquire(_surface, -1);
                if (index < 0 || uint32_t(index) >= _bufferCount) return;
                // The slide, already drawn, is copied by the 2D-DMA (its cache written back as it is
                // given); by the CPU if that fails.
                bmds_surface_image_t image = {
                        sizeof image,   0, canvas.pixels(), uint32_t(canvas.width()), uint32_t(canvas.height()),
                        canvas.stride()};
                bmds_surface_piece_t piece = {0, 0, uint32_t(_width), uint32_t(_height), 0, 0};
                bmds_surface_blit_t  blit = {sizeof blit, uint32_t(index), BMDS_BLIT_IMAGE, 0, 1, &piece};
                bool copied = bmds_surface_control(_surface, BMDS_SURFACE_IMAGE, &image, sizeof image) == 0 &&
                              bmds_surface_control(_surface, BMDS_SURFACE_BLIT, &blit, sizeof blit) == 0;
                if (!copied) {
                        uint8_t *to = static_cast<uint8_t *>(_buffers[index]);
                        uint32_t bytes = uint32_t(_width) * 2;
                        for (int32_t y = 0; y < _height; y++) {
                                memcpy(to + size_t(y) * _strides[index], canvas.pixels() + size_t(y) * canvas.stride(),
                                       bytes);
                        }
                }
                bmds_surface_present_t p = {sizeof p, uint32_t(index), nullptr, 0};
                {
                        int32_t shown = bmds_surface_present(_surface, &p);
                        if (shown >= 0) _shownFrame = uint32_t(shown);
                }
                return;
        }

        // ---- Frames of pieces: the 2D-DMA's copies from two registered pictures (slots 1 and 2;
        // present() uses 0). The CPU copies PSRAM at about 28 MB/s each way, the DMA at about 150:
        // a wipe's frame took 53 ms by the CPU (devplan/preso.md §14).

        bool BmdsPlatform::beginPieces(const Canvas &from, const Canvas &to) {
                (void)Platform::beginPieces(from, to); // the CPU's way, should the DMA refuse
                if (_surface < 0) return false;
                bmds_surface_image_t a = {
                        sizeof a, 1, from.pixels(), uint32_t(from.width()), uint32_t(from.height()), from.stride()};
                bmds_surface_image_t b = {sizeof b,   2, to.pixels(), uint32_t(to.width()), uint32_t(to.height()),
                                          to.stride()};
                _dmaPieces = bmds_surface_control(_surface, BMDS_SURFACE_IMAGE, &a, sizeof a) == 0 &&
                             bmds_surface_control(_surface, BMDS_SURFACE_IMAGE, &b, sizeof b) == 0;
                return true;
        }

        bool BmdsPlatform::presentPieces(const Piece *pieces, uint32_t count) {
                if (!_dmaPieces) return Platform::presentPieces(pieces, count);
                int32_t index = bmds_surface_acquire(_surface, -1);
                if (index < 0 || uint32_t(index) >= _bufferCount) return false;
                // Runs of pieces of one source (one fill colour) are a job each, in order, so later
                // pieces land over earlier ones.
                bmds_surface_piece_t run[BMDS_SURFACE_BLIT_MAX];
                uint32_t             n = 0;
                uint32_t             source = 0, from = 0;
                bool                 ok = true;
                auto                 flush = [&]() {
                        if (n == 0) return;
                        bmds_surface_blit_t blit = {sizeof blit, uint32_t(index), source, from, n, run};
                        if (bmds_surface_control(_surface, BMDS_SURFACE_BLIT, &blit, sizeof blit) != 0) ok = false;
                        n = 0;
                };
                for (uint32_t i = 0; i < count; i++) {
                        const Piece &p = pieces[i];
                        if (p.width <= 0 || p.height <= 0) continue;
                        uint32_t s = p.source == Piece::Fill ? BMDS_BLIT_FILL : BMDS_BLIT_IMAGE;
                        uint32_t f = p.source == Piece::Fill ? ((p.color >> 8) & 0xF800) | ((p.color >> 5) & 0x07E0) |
                                                                       ((p.color >> 3) & 0x001F)
                                     : p.source == Piece::From ? 1u
                                                               : 2u;
                        if (n > 0 && (s != source || f != from || n == BMDS_SURFACE_BLIT_MAX)) flush();
                        source = s;
                        from = f;
                        run[n++] =
                                bmds_surface_piece_t{p.x, p.y, uint32_t(p.width), uint32_t(p.height), p.fromX, p.fromY};
                }
                flush();
                // The damage: the pieces (a superset of what differs from the last frame), so the
                // system takes only that to the display. Without it a present took 12 ms a frame.
                // More than a present takes: runs of them as their bounds.
                constexpr uint32_t MaxDamage = 64;
                bmds_rect_t        damage[MaxDamage];
                uint32_t           d = 0, used = 0;
                for (uint32_t i = 0; i < count; i++) used += pieces[i].width > 0 && pieces[i].height > 0;
                uint32_t group = used <= MaxDamage ? 1 : (used + MaxDamage - 1) / MaxDamage, in = 0;
                for (uint32_t i = 0; i < count; i++) {
                        const Piece &p = pieces[i];
                        if (p.width <= 0 || p.height <= 0) continue;
                        if (in == 0) {
                                damage[d++] = bmds_rect_t{p.x, p.y, uint32_t(p.width), uint32_t(p.height)};
                        } else {
                                bmds_rect_t &r = damage[d - 1];
                                int32_t      x1 =
                                        r.x + int32_t(r.width) > p.x + p.width ? r.x + int32_t(r.width) : p.x + p.width;
                                int32_t y1 = r.y + int32_t(r.height) > p.y + p.height ? r.y + int32_t(r.height)
                                                                                      : p.y + p.height;
                                r.x = r.x < p.x ? r.x : p.x;
                                r.y = r.y < p.y ? r.y : p.y;
                                r.width = uint32_t(x1 - r.x);
                                r.height = uint32_t(y1 - r.y);
                        }
                        in = in + 1 == group ? 0 : in + 1;
                }
                bmds_surface_present_t present = {sizeof present, uint32_t(index), d > 0 ? damage : nullptr, d};
                {
                        int32_t shown = bmds_surface_present(_surface, &present);
                        if (shown >= 0) _shownFrame = uint32_t(shown);
                }
                return ok;
        }

        void BmdsPlatform::endPieces() {
                if (_dmaPieces) {
                        bmds_surface_image_t none = {sizeof none, 1, nullptr, 0, 0, 0};
                        (void)bmds_surface_control(_surface, BMDS_SURFACE_IMAGE, &none, sizeof none);
                        none.slot = 2;
                        (void)bmds_surface_control(_surface, BMDS_SURFACE_IMAGE, &none, sizeof none);
                        _dmaPieces = false;
                }
                Platform::endPieces();
                return;
        }

        bool BmdsPlatform::beginFrame(Canvas &canvas) {
                if (_surface < 0) return false;
                // A buffer free (with two, the one shown before this): waiting for it paces frames to
                // the display.
                int32_t index = bmds_surface_acquire(_surface, -1);
                if (index < 0 || uint32_t(index) >= _bufferCount) return false;
                _frame = index;
                canvas.attach(_buffers[index], _strides[index], _width, _height, PixelFormat::Rgb565);
                return true;
        }

        void BmdsPlatform::endFrame() {
                if (_frame < 0) return;
                bmds_surface_present_t p = {sizeof p, uint32_t(_frame), nullptr, 0};
                {
                        int32_t shown = bmds_surface_present(_surface, &p);
                        if (shown >= 0) _shownFrame = uint32_t(shown);
                }
                _frame = -1;
                return;
        }

        void BmdsPlatform::setInputMode(InputMode mode) {
                _mode = mode;
                if (_controls) {
                        const char *set = mode == InputMode::Overview ? "overview" : "deck";
                        (void)bmds_controls(BMDS_CONTROLS_SET, const_cast<char *>(set), uint32_t(strlen(set)));
                }
                return;
        }

        void BmdsPlatform::waitFrame() {
                if (_surface < 0) return;
                bmds_surface_wait_t w = {sizeof w, _shownFrame + 1, 100};
                if (bmds_surface_control(_surface, BMDS_SURFACE_WAIT, &w, sizeof w) == 0) _shownFrame++;
                return;
        }

        int64_t BmdsPlatform::now() {
                int64_t ns = 0;
                (void)bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
                return ns;
        }

        Error BmdsPlatform::watch(const char *path) {
                _watch = bmds_fs_watch(path, BMDS_O_CLOEXEC);
                return _watch >= 0 ? Error::Ok : Error::NotSupported;
        }

        void BmdsPlatform::dispatch(App &app) {
                bmds_surface_event_t events[16];
                int32_t              n = bmds_read(_surface, events, sizeof events);
                if (n <= 0) return;
                for (uint32_t i = 0; i < uint32_t(n) / sizeof(bmds_surface_event_t) && !_quit; i++) {
                        const bmds_surface_event_t &e = events[i];
                        Event                       out;
                        if (e.type == BMDS_EVENT_ACTION) {
                                if (e.flags != BMDS_ACTION_PRESS && e.flags != BMDS_ACTION_REPEAT) continue;
                                if (e.data[0] < uint32_t(Action::Next) || e.data[0] > uint32_t(Action::Close)) continue;
                                out.type = Event::Type::Action;
                                out.action = Action(e.data[0]);
                                out.repeat = e.flags == BMDS_ACTION_REPEAT;
                        } else if (e.type == BMDS_EVENT_KEY) {
                                if ((e.flags & (BMDS_KEY_DOWN | BMDS_KEY_REPEAT)) == 0) continue;
                                Action a = _controls ? Action::None : actionForKey(e.data[0], _mode);
                                if (a != Action::None) {
                                        out.type = Event::Type::Action;
                                        out.action = a;
                                } else {
                                        out.type = Event::Type::Key;
                                        out.key = e.data[0];
                                        out.character = e.data[2];
                                }
                                out.repeat = (e.flags & BMDS_KEY_REPEAT) != 0;
                        } else if (e.type == BMDS_EVENT_CLOSE) {
                                out.type = Event::Type::Close;
                        } else {
                                continue;
                        }
                        app.event(out);
                }
                return;
        }

        Error BmdsPlatform::run(App &app) {
                if (_surface < 0) return Error::NotSupported;
                // A save is often several writes (or a write and a rename): the deck is read again
                // once they have stopped for a moment.
                constexpr int32_t Settle = 150;
                bool              changed = false;
                _quit = false;
                while (!_quit) {
                        bmds_pollfd_t fds[2] = {{sizeof(bmds_pollfd_t), _surface, BMDS_POLL_IN, 0},
                                                {sizeof(bmds_pollfd_t), _watch, BMDS_POLL_IN, 0}};
                        // Animating, nothing is waited for here: the frame waits for its buffer.
                        int32_t timeout = _animating ? 0 : changed ? Settle : -1;
                        int32_t r = bmds_io_poll(fds, 2, timeout, nullptr);
                        if (r < 0 && r != -BMDS_EINTR) return Error::Io;
                        if (r == 0 && changed && !_animating) {
                                changed = false;
                                app.filesChanged();
                                continue;
                        }
                        if ((fds[1].revents & BMDS_POLL_IN) != 0) {
                                uint32_t what = 0;
                                (void)bmds_read(_watch, &what, sizeof what);
                                changed = true;
                        }
                        if ((fds[0].revents & BMDS_POLL_IN) != 0) dispatch(app);
                        if ((fds[0].revents & (BMDS_POLL_HUP | BMDS_POLL_ERR)) != 0) break;
                        if (_animating && !_quit) app.frame(now());
                }
                return Error::Ok;
        }

} // namespace preso
