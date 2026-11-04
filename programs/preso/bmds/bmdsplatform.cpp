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
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <bmds/game.h>
#include <bmds/sys.h>

extern char **environ; // a demo's environment: Preso's own

namespace preso {

        namespace {

                // The presenter's actions (devplan/controls.md §7c): the defaults, which the user
                // may change in Controls. The Ids are Preso's Action numbers. Four keys an action at
                // most; Enter is left a key (Next, or the end of a slide's number: step 5).
                constexpr char Controls[] =
                        "[Controls]\n"
                        "Sets=deck;overview;script\n"
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
                        "Id=14\nLabel=Back\nGroup=Overview\nSets=overview\nKeys=Tab;Escape\nPad=Pad.East;Pad.Start\n"
                        // A script region's (§7.10): entered, every other key is the script's, and a
                        // clicker's keys still step.
                        "[Action into]\n"
                        "Id=15\nLabel=Into the slide's piece\nGroup=Scripts\nSets=deck\nKeys=I\nPad=Pad.West\n"
                        "[Action leave]\n"
                        "Id=16\nLabel=Out of the piece\nGroup=Scripts\nSets=script\nKeys=Escape\nPad=Pad.East\n"
                        "[Action clearink]\n"
                        "Id=17\nLabel=Clear the ink\nGroup=Presenting\nSets=deck\nKeys=C\nPad=Pad.North\n"
                        "[Action stepnext]\n"
                        "Id=32\nLabel=Next, from a piece\nGroup=Scripts\nSets=script\nKeys=PageDown\nPad=Pad.R1\n"
                        "[Action stepback]\n"
                        "Id=33\nLabel=Previous, from a piece\nGroup=Scripts\nSets=script\nKeys=PageUp\nPad=Pad.L1\n";

                int childWrite = -1; // SIGCHLD's handler's end of the pipe (a demo's end, §6a)

                static_assert(uint32_t(Action::Next) == 1 && uint32_t(Action::Previous) == 2 &&
                                      uint32_t(Action::First) == 3 && uint32_t(Action::Last) == 4 &&
                                      uint32_t(Action::Quit) == 5 && uint32_t(Action::Black) == 6 &&
                                      uint32_t(Action::White) == 7 && uint32_t(Action::Overview) == 8 &&
                                      uint32_t(Action::Left) == 9 && uint32_t(Action::Close) == 14 &&
                                      uint32_t(Action::Into) == 15 && uint32_t(Action::Leave) == 16 &&
                                      uint32_t(Action::ClearInk) == 17,
                              "Controls' Ids are the actions");

        } // namespace

        BmdsPlatform::~BmdsPlatform() {
                restoreTerminal();
                if (_childPipe[0] >= 0) {
                        signal(SIGCHLD, SIG_DFL);
                        childWrite = -1;
                        close(_childPipe[0]);
                        close(_childPipe[1]);
                }
                if (_audio != nullptr) {
                        bmds_game_audio_close(static_cast<bmds_game_audio_t *>(_audio));
                        for (Sound &s : _sounds) {
                                if (s.loaded == nullptr) continue;
                                bmds_game_sound_free(static_cast<bmds_game_sound_t *>(s.loaded));
                                free(s.loaded);
                        }
                }
                for (uint32_t i = 0; i < _watchCount; i++) bmds_close(_watches[i]);
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
                // Not a demo's (§6a): kept by its shell, the screen wouldn't close for it.
                (void)bmds_fd_flags(_surface, BMDS_FD_CLOEXEC);
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
                // The presenter's actions; without them (an older system), keys by their usages. The
                // process's: not asked for again when the screen opens again after a demo.
                if (!_declared) {
                        _declared = true;
                        int32_t r =
                                bmds_controls(BMDS_CONTROLS_DECLARE, const_cast<char *>(Controls), sizeof Controls - 1);
                        if (r == 0) r = bmds_controls(BMDS_CONTROLS_SET, const_cast<char *>("deck"), 4);
                        _controls = r == 0;
                        if (!_controls && r != -BMDS_ENOSYS) fprintf(stderr, "preso: its controls: %s\n", strerror(-r));
                }
                // No arrow over the slides (the presenter's pointer is Preso's own: step 8).
                uint32_t pointer = BMDS_POINTER_HIDDEN;
                (void)bmds_surface_control(_surface, BMDS_SURFACE_POINTER, &pointer, sizeof pointer);
                return Error::Ok;
        }

        void BmdsPlatform::closeScreen() {
                // Its buffers given back with it (a demo has the screen: §6a).
                if (_surface < 0) return;
                bmds_close(_surface);
                _surface = -1;
                _bufferCount = 0;
                _frame = -1;
                _dmaPieces = false;
                for (uint32_t i = 0; i < 3; i++) {
                        _buffers[i] = nullptr;
                        _stale[i] = Rect();
                }
                return;
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
                // The other buffers hold older slides now.
                for (uint32_t i = 0; i < _bufferCount; i++) {
                        _stale[i] = i == uint32_t(index) ? Rect{} : Rect{0, 0, _width, _height};
                }
                return;
        }

        void BmdsPlatform::staleAll() {
                for (uint32_t i = 0; i < _bufferCount; i++) _stale[i] = Rect{0, 0, _width, _height};
                return;
        }

        void BmdsPlatform::presentAreas(const Canvas &canvas, const Rect *areas, uint32_t count) {
                // A script region's frame: only what changed copied (a piece each), into a buffer that
                // also lacks what changed while another was shown, and the display told only that.
                if (_surface < 0) return;
                int32_t index = bmds_surface_acquire(_surface, -1);
                if (index < 0 || uint32_t(index) >= _bufferCount) return;
                Rect                 screen{0, 0, _width, _height};
                bmds_surface_piece_t pieces[BMDS_SURFACE_BLIT_MAX];
                bmds_rect_t          damage[BMDS_SURFACE_BLIT_MAX];
                uint32_t             n = 0;
                Rect                 all; // every area, which the other buffers now lack
                auto                 add = [&](const Rect &area) {
                        Rect r = area.intersected(screen);
                        if (r.isEmpty()) return;
                        if (n == BMDS_SURFACE_BLIT_MAX) {
                                // No room: the last piece grows to hold it.
                                Rect last{pieces[n - 1].x, pieces[n - 1].y, int32_t(pieces[n - 1].width),
                                          int32_t(pieces[n - 1].height)};
                                r = uniteRects(last, r);
                                n--;
                        }
                        pieces[n] = bmds_surface_piece_t{r.x, r.y, uint32_t(r.width), uint32_t(r.height), r.x, r.y};
                        damage[n] = bmds_rect_t{r.x, r.y, uint32_t(r.width), uint32_t(r.height)};
                        n++;
                };
                add(_stale[index]);
                for (uint32_t i = 0; i < count; i++) {
                        add(areas[i]);
                        all = uniteRects(all, areas[i]);
                }
                for (uint32_t i = 0; i < _bufferCount; i++) {
                        _stale[i] = i == uint32_t(index) ? Rect{} : uniteRects(_stale[i], all);
                }
                if (n > 0) {
                        bmds_surface_image_t image = {
                                sizeof image,   0, canvas.pixels(), uint32_t(canvas.width()), uint32_t(canvas.height()),
                                canvas.stride()};
                        bmds_surface_blit_t blit = {sizeof blit, uint32_t(index), BMDS_BLIT_IMAGE, 0, n, pieces};
                        bool copied = bmds_surface_control(_surface, BMDS_SURFACE_IMAGE, &image, sizeof image) == 0 &&
                                      bmds_surface_control(_surface, BMDS_SURFACE_BLIT, &blit, sizeof blit) == 0;
                        if (!copied) {
                                uint8_t *to = static_cast<uint8_t *>(_buffers[index]);
                                for (uint32_t k = 0; k < n; k++) {
                                        const bmds_surface_piece_t &p = pieces[k];
                                        for (int32_t y = p.y; y < p.y + int32_t(p.height); y++) {
                                                memcpy(to + size_t(y) * _strides[index] + size_t(p.x) * 2,
                                                       canvas.pixels() + size_t(y) * canvas.stride() + size_t(p.x) * 2,
                                                       size_t(p.width) * 2);
                                        }
                                }
                        }
                }
                bmds_surface_present_t p = {sizeof p, uint32_t(index), n > 0 ? damage : nullptr, n};
                int32_t                shown = bmds_surface_present(_surface, &p);
                if (shown >= 0) _shownFrame = uint32_t(shown);
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
                staleAll(); // a transition's frames: the slide presented again after
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
                staleAll();
                return;
        }

        void BmdsPlatform::setInputMode(InputMode mode) {
                _mode = mode;
                if (_controls) {
                        const char *set = mode == InputMode::Overview ? "overview"
                                          : mode == InputMode::Script ? "script"
                                                                      : "deck";
                        (void)bmds_controls(BMDS_CONTROLS_SET, const_cast<char *>(set), uint32_t(strlen(set)));
                }
                return;
        }

        // ---- Live demos (devplan/preso.md §6a): a command through the shell, the deck out of its way

        namespace {

                void childEnded(int) {
                        int  saved = errno;
                        char b = 0;
                        if (childWrite >= 0) (void)write(childWrite, &b, 1);
                        errno = saved;
                }

        } // namespace

        Error BmdsPlatform::startDemo(const char *command, const char *folder) {
                if (_demoPid >= 0) return Error::Invalid; // one at a time
                if (_childPipe[0] < 0) {
                        // Children's ends heard without polling: a byte in a pipe the loop watches.
                        if (pipe2(_childPipe, O_CLOEXEC | O_NONBLOCK) != 0) return Error::Io;
                        childWrite = _childPipe[1];
                        struct sigaction a = {};
                        a.sa_handler = childEnded;
                        a.sa_flags = SA_RESTART;
                        sigemptyset(&a.sa_mask);
                        sigaction(SIGCHLD, &a, nullptr);
                }
                // From the deck's folder: `cd 'FOLDER' && COMMAND`, the folder quoted.
                char line[1200];
                int  n = snprintf(line, sizeof line, "cd '");
                for (const char *p = folder; *p != '\0' && n < int(sizeof line) - 8; p++) {
                        n += *p == '\'' ? snprintf(line + n, sizeof line - size_t(n), "'\\''")
                                        : snprintf(line + n, sizeof line - size_t(n), "%c", *p);
                }
                n += snprintf(line + n, sizeof line - size_t(n), "' && %s", command);
                if (n >= int(sizeof line)) return Error::TooBig;
                char *argv[] = {const_cast<char *>("sh"), const_cast<char *>("-c"), line, nullptr};
                pid_t pid = -1;
                if (posix_spawnp(&pid, "sh", nullptr, nullptr, argv, ::environ) != 0) return Error::NotFound;
                _demoPid = int32_t(pid);
                return Error::Ok;
        }

        void BmdsPlatform::demoFinished(App &app) {
                // A byte from SIGCHLD's handler: whether it was the demo that ended.
                char bytes[16];
                while (read(_childPipe[0], bytes, sizeof bytes) > 0) {}
                if (_demoPid < 0) return;
                int   status = 0;
                pid_t r = waitpid(pid_t(_demoPid), &status, WNOHANG);
                if (r != pid_t(_demoPid)) return;
                _demoPid = -1;
                app.demoEnded(WIFEXITED(status) ? WEXITSTATUS(status)
                                                : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0));
                return;
        }

        // ---- The terminal it was started from (over SSH: the laptop's)

        bool BmdsPlatform::readTerminal() {
                if (_terminal || !isatty(0)) return false;
                auto *saved = static_cast<struct termios *>(malloc(sizeof(struct termios)));
                if (saved == nullptr || tcgetattr(0, saved) != 0) {
                        free(saved);
                        return false;
                }
                // Each key as it is typed, not echoed (the notes are on that screen).
                struct termios raw = *saved;
                // Ctrl-C too: a key that quits as `q` does, the terminal put back (not a signal).
                raw.c_lflag &= ~tcflag_t(ICANON | ECHO | ISIG);
                raw.c_cc[VMIN] = 1;
                raw.c_cc[VTIME] = 0;
                if (tcsetattr(0, TCSANOW, &raw) != 0) {
                        free(saved);
                        return false;
                }
                _savedTerminal = saved;
                _terminal = true;
                return true;
        }

        void BmdsPlatform::restoreTerminal() {
                if (!_terminal) return;
                (void)tcsetattr(0, TCSANOW, static_cast<struct termios *>(_savedTerminal));
                free(_savedTerminal);
                _savedTerminal = nullptr;
                _terminal = false;
                return;
        }

        bool BmdsPlatform::outputIsTerminal() {
                return isatty(1) != 0;
        }

        void BmdsPlatform::setPointerVisible(bool visible) {
                if (_surface < 0) return;
                uint32_t pointer = visible ? BMDS_POINTER_NORMAL : BMDS_POINTER_HIDDEN;
                (void)bmds_surface_control(_surface, BMDS_SURFACE_POINTER, &pointer, sizeof pointer);
                return;
        }

        // ---- Sounds: libbmdsgame's mixer (the device opened with the first sound)

        void BmdsPlatform::freeQuietSounds() {
                for (Sound &s : _sounds) {
                        if (!s.closed || s.loaded == nullptr) continue;
                        if (s.voice >= 0 &&
                            bmds_game_voice_is_playing(static_cast<bmds_game_audio_t *>(_audio), s.voice))
                                continue;
                        auto *sound = static_cast<bmds_game_sound_t *>(s.loaded);
                        bmds_game_sound_free(sound);
                        free(sound);
                        s = Sound();
                }
                return;
        }

        int32_t BmdsPlatform::openSound(const char *path) {
                if (_audio == nullptr && !_audioFailed) {
                        bmds_game_audio_t *audio = nullptr;
                        if (bmds_game_audio_open(nullptr, &audio) == 0) {
                                _audio = audio;
                        } else {
                                _audioFailed = true;
                        }
                }
                if (_audio == nullptr) return -1;
                freeQuietSounds();
                for (uint32_t i = 0; i < MaxSounds; i++) {
                        if (_sounds[i].loaded != nullptr) continue;
                        auto *sound = static_cast<bmds_game_sound_t *>(calloc(1, sizeof(bmds_game_sound_t)));
                        if (sound == nullptr) return -1;
                        if (bmds_game_sound_load(static_cast<bmds_game_audio_t *>(_audio), path, sound) != 0) {
                                free(sound);
                                return -1;
                        }
                        _sounds[i].loaded = sound;
                        _sounds[i].voice = -1;
                        _sounds[i].closed = false;
                        return int32_t(i);
                }
                return -1;
        }

        void BmdsPlatform::playSound(int32_t sound, float volume) {
                if (_audio == nullptr || sound < 0 || uint32_t(sound) >= MaxSounds) return;
                Sound &s = _sounds[sound];
                if (s.loaded == nullptr || s.closed) return;
                int32_t voice = bmds_game_play(static_cast<bmds_game_audio_t *>(_audio),
                                               static_cast<bmds_game_sound_t *>(s.loaded), volume, 0, 0);
                if (voice >= 0) s.voice = voice;
                return;
        }

        void BmdsPlatform::closeSound(int32_t sound) {
                if (_audio == nullptr || sound < 0 || uint32_t(sound) >= MaxSounds) return;
                _sounds[sound].closed = true;
                freeQuietSounds();
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
                // Each folder its own watch (the deck's, its scripts' and pictures' folders).
                if (_watchCount == MaxWatches) return Error::NoMemory;
                int32_t fd = bmds_fs_watch(path, BMDS_O_CLOEXEC);
                if (fd < 0) return Error::NotSupported;
                _watches[_watchCount++] = fd;
                return Error::Ok;
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
                                // A script region's stepping keys are Next and Previous.
                                uint32_t id = e.data[0] == 17   ? uint32_t(Action::Next)
                                              : e.data[0] == 18 ? uint32_t(Action::Previous)
                                                                : e.data[0];
                                if (id < uint32_t(Action::Next) || id > uint32_t(Action::Leave)) continue;
                                out.type = Event::Type::Action;
                                out.action = Action(id);
                                out.repeat = e.flags == BMDS_ACTION_REPEAT;
                        } else if (e.type == BMDS_EVENT_POINTER) {
                                int32_t x = int32_t(e.data[0]), y = int32_t(e.data[1]);
                                if (x == BMDS_POINTER_GONE) continue;
                                out.type = Event::Type::Pointer;
                                out.x = _pointerX = x;
                                out.y = _pointerY = y;
                                out.buttons = e.data[2];
                        } else if (e.type == BMDS_EVENT_PAD && e.flags == BMDS_PAD_AXIS &&
                                   (e.data[1] == BMDS_PAD_LEFT_X || e.data[1] == BMDS_PAD_LEFT_Y)) {
                                // The left stick: where it now leans, both ways.
                                if (e.data[1] == BMDS_PAD_LEFT_X) {
                                        _stickX = int32_t(e.data[2]);
                                } else {
                                        _stickY = int32_t(e.data[2]);
                                }
                                out.type = Event::Type::Stick;
                                out.x = _stickX;
                                out.y = _stickY;
                        } else if (e.type == BMDS_EVENT_WHEEL) {
                                out.type = Event::Type::Wheel;
                                out.wheel = int32_t(e.data[0]);
                                out.x = _pointerX;
                                out.y = _pointerY;
                        } else if (e.type == BMDS_EVENT_KEY && (e.flags & BMDS_KEY_UP) != 0) {
                                // Let go: only an entered script region wants to know.
                                if (_mode != InputMode::Script) continue;
                                out.type = Event::Type::KeyUp;
                                out.key = e.data[0];
                        } else if (e.type == BMDS_EVENT_KEY) {
                                if ((e.flags & (BMDS_KEY_DOWN | BMDS_KEY_REPEAT)) == 0) continue;
                                Action a = _controls ? Action::None : defaultAction(e.data[0], _mode);
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
                        // The surface, the terminal's keys, the folders watched.
                        // POSIX poll, not bmds_io_poll: libc runs the signal handlers as a call
                        // returns (SIGCHLD's, a demo's end), the raw call leaves them to its caller.
                        struct pollfd fds[3 + MaxWatches];
                        uint32_t      n = 0;
                        fds[n++] = pollfd{_surface, POLLIN, 0};
                        int32_t terminal = -1;
                        if (_terminal) {
                                terminal = int32_t(n);
                                fds[n++] = pollfd{0, POLLIN, 0};
                        }
                        int32_t child = -1;
                        if (_childPipe[0] >= 0) {
                                child = int32_t(n);
                                fds[n++] = pollfd{_childPipe[0], POLLIN, 0};
                        }
                        uint32_t watchesAt = n;
                        for (uint32_t i = 0; i < _watchCount; i++) fds[n++] = pollfd{_watches[i], POLLIN, 0};
                        // Animating, nothing is waited for here: the frame waits for its buffer.
                        int32_t timeout = _animating ? 0 : changed ? Settle : -1;
                        int64_t ticked = now();
                        if (_ticking && !_animating) {
                                if (_nextSecond <= ticked) _nextSecond = ticked + 1000000000;
                                int32_t untilSecond = int32_t((_nextSecond - ticked) / 1000000) + 1;
                                if (timeout < 0 || untilSecond < timeout) timeout = untilSecond;
                        }
                        int r = ::poll(fds, nfds_t(n), timeout);
                        if (r < 0 && errno != EINTR) return Error::Io;
                        if (_ticking && now() >= _nextSecond) {
                                _nextSecond = now() + 1000000000;
                                app.second();
                        }
                        if (r == 0 && changed && !_animating) {
                                changed = false;
                                app.filesChanged();
                                continue;
                        }
                        for (uint32_t i = 0; i < _watchCount; i++) {
                                if ((fds[watchesAt + i].revents & POLLIN) == 0) continue;
                                uint32_t what = 0;
                                (void)bmds_read(_watches[i], &what, sizeof what);
                                changed = true;
                        }
                        if (child >= 0 && (fds[child].revents & POLLIN) != 0) demoFinished(app);
                        if (terminal >= 0 && (fds[terminal].revents & (POLLIN | POLLHUP)) != 0) {
                                char    bytes[64];
                                ssize_t got = read(0, bytes, sizeof bytes);
                                if (got > 0) {
                                        app.terminalInput(bytes, uint32_t(got));
                                } else {
                                        restoreTerminal(); // the connection gone: no more keys from it
                                }
                        }
                        if ((fds[0].revents & POLLIN) != 0) dispatch(app);
                        if ((fds[0].revents & (POLLHUP | POLLERR)) != 0) break;
                        if (_animating && !_quit) app.frame(now());
                }
                return Error::Ok;
        }

} // namespace preso
