/**
 * @file      scancompositor.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The scan-time composition prototype (devplan/windows.md §4b).
 */

#include <cstring>
#include <driver/ppa.h>
#include <esp_cache.h>
#include <esp_cpu.h>
#include <bmds/memory.h>
#include <bmds/scancompositor.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t       Align = 128;                // the L2 cache line; the 2D-DMA's PSRAM alignment
        constexpr size_t       WriteBackChunk = 64 * 1024; // a cache operation runs with interrupts off
        constexpr unsigned int JobTimeoutMs = 20;
        constexpr uint16_t     Clear = 0xf81f; // the sprite's color key: magenta

        // The test layers' colors.
        constexpr uint32_t Colors[8] = {0xe04040, 0x40c040, 0x4060e0, 0xe0c040, 0x40c0c0, 0xc040c0, 0xe08040, 0x8080e0};

        uint16_t rgb565(uint32_t rgb) {
                return static_cast<uint16_t>((rgb >> 19 & 31) << 11 | (rgb >> 10 & 63) << 5 | (rgb >> 3 & 31));
        }

        // RGB565 as RGB888, the low bits zero (only the top bits are compared).
        uint32_t rgb888(uint16_t p) {
                return uint32_t(p >> 11 & 31) << 19 | uint32_t(p >> 5 & 63) << 10 | uint32_t(p & 31) << 3;
        }

        void writeBack(uint8_t *data, size_t bytes) {
                for (size_t off = 0; off < bytes; off += WriteBackChunk)
                        (void)esp_cache_msync(data + off, bytes - off < WriteBackChunk ? bytes - off : WriteBackChunk,
                                              ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                return;
        }

        // A test layer's pixel: a white border, a black square at the top-left
        // (which way up), the layer's color darkening downwards.
        uint32_t patternPixel(uint8_t pattern, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
                if (x < 4 || y < 4 || x >= w - 4 || y >= h - 4) return 0xffffff;
                if (x < 24 && y < 24) return 0x000000;
                const uint32_t c = Colors[pattern % 8];
                const uint32_t k = 256 - (uint32_t(y) * 160 / h);
                return ((c >> 16 & 255) * k >> 8) << 16 | ((c >> 8 & 255) * k >> 8) << 8 | ((c & 255) * k >> 8);
        }

        uint32_t cycles() {
                return esp_cpu_get_cycle_count();
        }

        void account(uint64_t &sum, uint32_t &max, uint32_t value) {
                sum += value;
                if (value > max) max = value;
                return;
        }

} // namespace

Error ScanCompositor::start(uint16_t width, uint16_t height, uint32_t desktopColor) {
        if (_desktop != nullptr) return Error::InvalidState;
        if (width == 0 || (width & 1) != 0 || height == 0) return Error::Invalid;
        _width = width;
        _height = height;
        _desktopColor = desktopColor;
        const size_t desktopBytes = size_t(width) * height * 2;
        _desktop = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::PsramDma, desktopBytes, Align));
        _sprite = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::PsramDma, SpriteSize * SpriteSize * 2, Align));
        if (_desktop == nullptr || _sprite == nullptr) {
                stop();
                return Error::NoMem;
        }
        auto          *d = reinterpret_cast<uint16_t *>(_desktop);
        const uint16_t c = rgb565(desktopColor);
        for (size_t i = 0; i < size_t(width) * height; ++i) d[i] = c;
        writeBack(_desktop, desktopBytes);
        // An arrow: a black outline, white inside, the rest clear.
        auto *s = reinterpret_cast<uint16_t *>(_sprite);
        for (uint16_t y = 0; y < SpriteSize; ++y) {
                for (uint16_t x = 0; x < SpriteSize; ++x) {
                        uint16_t p = Clear;
                        if (x <= y && y < 24 && x < 16) p = (x == 0 || x == y || y == 23 || x == 15) ? 0x0000 : 0xffff;
                        s[y * SpriteSize + x] = p;
                }
        }
        writeBack(_sprite, SpriteSize * SpriteSize * 2);

        Error err = _chain565.start(BlitChain::Conversion::Rgb565ToRgb888, MaxPieces);
        if (err.isOk()) err = _chain888.start(BlitChain::Conversion::None, MaxPieces);
        ppa_client_config_t pc = {};
        pc.oper_type = PPA_OPERATION_FILL;
        pc.max_pending_trans_num = 1;
        if (err.isOk()) {
                ppa_client_handle_t h = nullptr;
                err = Error::fromEspErr(ppa_register_client(&pc, &h));
                _fillClient = h;
        }
        pc.oper_type = PPA_OPERATION_BLEND;
        if (err.isOk()) {
                ppa_client_handle_t h = nullptr;
                err = Error::fromEspErr(ppa_register_client(&pc, &h));
                _blendClient = h;
        }
        if (err.isError()) stop();
        // Each start from the defaults: the desktop as a surface, chained jobs, no sprite.
        _wantBackground.store(Background::Surface);
        _wantJobs.store(Jobs::Chained);
        _wantSprite.store(0);
        _background = Background::Surface;
        _jobs = Jobs::Chained;
        _spriteShown = false;
        _stats = Stats();
        return err;
}

void ScanCompositor::stop() {
        _chain565.stop();
        _chain888.stop();
        if (_fillClient != nullptr) (void)ppa_unregister_client(static_cast<ppa_client_handle_t>(_fillClient));
        if (_blendClient != nullptr) (void)ppa_unregister_client(static_cast<ppa_client_handle_t>(_blendClient));
        _fillClient = nullptr;
        _blendClient = nullptr;
        for (size_t i = 0; i < _layerCount; ++i) {
                Memory::free(_layers[i].data);
                _layers[i].data = nullptr;
                _layers[i].dx = _layers[i].dy = 0;
        }
        _layerCount = 0;
        Memory::free(_desktop);
        Memory::free(_sprite);
        _desktop = nullptr;
        _sprite = nullptr;
        return;
}

Result<uint8_t> ScanCompositor::addLayer(Format format, uint16_t width, uint16_t height, int x, int y,
                                         uint8_t pattern) {
        if (_desktop == nullptr) return makeError<uint8_t>(Error::InvalidState);
        if (_layerCount == MaxLayers) return makeError<uint8_t>(Error::Full);
        if (width < 32 || height < 32 || (width & 1) != 0 || width > _width || height > _height)
                return makeError<uint8_t>(Error::Invalid);
        const size_t bpp = format == Format::Rgb565 ? 2 : 3;
        const size_t bytes = size_t(width) * height * bpp;
        auto        *data = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::PsramDma, bytes, Align));
        if (data == nullptr) return makeError<uint8_t>(Error::NoMem);
        for (uint16_t row = 0; row < height; ++row) {
                for (uint16_t col = 0; col < width; ++col) {
                        const uint32_t rgb = patternPixel(pattern, col, row, width, height);
                        uint8_t       *p = data + (size_t(row) * width + col) * bpp;
                        if (bpp == 2) {
                                const uint16_t v = rgb565(rgb);
                                p[0] = static_cast<uint8_t>(v);
                                p[1] = static_cast<uint8_t>(v >> 8);
                        } else {
                                p[0] = static_cast<uint8_t>(rgb);
                                p[1] = static_cast<uint8_t>(rgb >> 8);
                                p[2] = static_cast<uint8_t>(rgb >> 16);
                        }
                }
        }
        writeBack(data, bytes);
        Layer &l = _layers[_layerCount];
        l.data = data;
        l.width = width;
        l.height = height;
        l.format = format;
        l.dx = l.dy = 0;
        l.x = static_cast<int16_t>(x & ~1);
        l.y = static_cast<int16_t>(y);
        l.wanted.store(pack(l.x, l.y));
        return makeResult(static_cast<uint8_t>(_layerCount++));
}

void ScanCompositor::moveLayer(size_t index, int x, int y) {
        if (index < _layerCount) _layers[index].wanted.store(pack(x & ~1, y));
        return;
}

void ScanCompositor::dragLayer(size_t index, int dx, int dy) {
        if (index >= _layerCount) return;
        _layers[index].dx = static_cast<int16_t>(dx & ~1);
        _layers[index].dy = static_cast<int16_t>(dy);
        return;
}

void ScanCompositor::setSprite(bool shown, int x, int y) {
        _wantSprite.store((shown ? 1u << 31 : 0) | uint32_t(x & 0x7ffe) << 15 | uint32_t(y & 0x7fff));
        return;
}

Rect ScanCompositor::onScreen(int x, int y, uint16_t width, uint16_t height) const {
        const int left = x < 0 ? 0 : x;
        const int top = y < 0 ? 0 : y;
        const int right = x + width > _width ? _width : x + width;
        const int bottom = y + height > _height ? _height : y + height;
        if (right <= left || bottom <= top) return Rect();
        return Rect{uint16_t(left), uint16_t(top), uint16_t(right - left), uint16_t(bottom - top)};
}

// The frame's positions and modes, taken at its start.
void ScanCompositor::beginFrame(uint32_t) {
        for (size_t i = 0; i < _layerCount; ++i) {
                Layer &l = _layers[i];
                if (l.dx != 0 || l.dy != 0) {
                        // Dragged: bounce off the screen's edges.
                        const uint32_t w = l.wanted.load();
                        int            x = int16_t(w >> 16) + l.dx;
                        int            y = int16_t(w & 0xffff) + l.dy;
                        if (x < 0 || x + l.width > _width) {
                                l.dx = static_cast<int16_t>(-l.dx);
                                x += 2 * l.dx;
                        }
                        if (y < 0 || y + l.height > _height) {
                                l.dy = static_cast<int16_t>(-l.dy);
                                y += 2 * l.dy;
                        }
                        l.wanted.store(pack(x, y));
                }
                const uint32_t w = l.wanted.load();
                l.x = static_cast<int16_t>(w >> 16);
                l.y = static_cast<int16_t>(w & 0xffff);
        }
        _background = _wantBackground.load();
        _jobs = _wantJobs.load();
        const uint32_t s = _wantSprite.load();
        _spriteShown = (s >> 31) != 0;
        _spriteX = static_cast<int16_t>(s >> 15 & 0x7fff);
        _spriteY = static_cast<int16_t>(s & 0x7fff);
        return;
}

void ScanCompositor::composeBlock(uint16_t first, uint16_t count, uint8_t *dst, size_t stride) {
        if (_resetWanted.exchange(false)) _stats = Stats();
        const uint32_t start = cycles();
        const Rect     band{0, first, _width, count};

        // The layers clipped to the block, the top one first; the desktop surface last.
        constexpr uint8_t Desktop = 0xff;
        Rect              rects[MaxLayers + 1];
        uint8_t           which[MaxLayers + 1];
        size_t            n = 0;
        for (size_t i = _layerCount; i-- > 0;) {
                const Layer &l = _layers[i];
                rects[n] = onScreen(l.x, l.y, l.width, l.height).intersected(band);
                which[n++] = static_cast<uint8_t>(i);
        }
        if (_background == Background::Surface) {
                rects[n] = band;
                which[n++] = Desktop;
        }
        Region damage;
        damage.add(band);
        auto [pieces, perr] = planCompose(Span<const Rect>(rects, n), damage, Span<ComposePiece>(_pieces, MaxPieces));
        const uint32_t planned = cycles();
        account(_stats.planCycles, _stats.planMax, planned - start);
        if (perr.isError()) {
                ++_stats.planFull; // the block goes out stale
                return;
        }

        uint32_t mark = planned;
        if (_background == Background::Fill) {
                ppa_fill_oper_config_t f = {};
                f.out.buffer = dst;
                f.out.buffer_size = static_cast<uint32_t>(stride * count);
                f.out.pic_w = _width;
                f.out.pic_h = count;
                f.out.fill_cm = PPA_FILL_COLOR_MODE_RGB888;
                f.fill_block_w = _width;
                f.fill_block_h = count;
                f.fill_argb_color.val = 0xff000000u | _desktopColor;
                f.mode = PPA_TRANS_MODE_BLOCKING;
                if (ppa_do_fill(static_cast<ppa_client_handle_t>(_fillClient), &f) != ESP_OK) ++_stats.errors;
                const uint32_t now = cycles();
                account(_stats.fillCycles, _stats.fillMax, now - mark);
                mark = now;
        }

        const BlitChain::Picture ring{dst, _width, count, 3};
        uint32_t                 jobs = 0;
        for (size_t i = 0; i < pieces; ++i) {
                const ComposePiece &p = _pieces[i];
                const uint8_t       index = which[p.layer];
                BlitChain::Picture  from;
                uint16_t            fx = p.rect.x, fy = p.rect.y;
                bool                wide = true; // RGB565, widened
                if (index == Desktop) {
                        from = BlitChain::Picture{_desktop, _width, _height, 2};
                } else {
                        const Layer &l = _layers[index];
                        wide = l.format == Format::Rgb565;
                        from = BlitChain::Picture{l.data, l.width, l.height, uint8_t(wide ? 2 : 3)};
                        fx = static_cast<uint16_t>(p.rect.x - l.x);
                        fy = static_cast<uint16_t>(p.rect.y - l.y);
                }
                BlitChain &chain = wide ? _chain565 : _chain888;
                if (chain.add(from, fx, fy, ring, p.rect.x, static_cast<uint16_t>(p.rect.y - first), p.rect.width,
                              p.rect.height)
                            .isError())
                        ++_stats.errors;
                if (_jobs == Jobs::Each) {
                        if (chain.run(JobTimeoutMs).isError()) ++_stats.errors;
                        ++jobs;
                }
        }
        if (_jobs == Jobs::Chained) {
                for (BlitChain *chain : {&_chain565, &_chain888}) {
                        if (chain->count() == 0) continue;
                        if (chain->run(JobTimeoutMs).isError()) ++_stats.errors;
                        ++jobs;
                }
        }
        uint32_t now = cycles();
        account(_stats.copyCycles, _stats.copyMax, now - mark);
        mark = now;

        // The sprite, blended over what's there with its color key.
        if (_spriteShown) {
                const Rect sprite = onScreen(_spriteX, _spriteY, SpriteSize, SpriteSize).intersected(band);
                if (!sprite.isEmpty()) {
                        ppa_blend_oper_config_t b = {};
                        b.in_bg.buffer = dst;
                        b.in_bg.pic_w = _width;
                        b.in_bg.pic_h = count;
                        b.in_bg.block_w = sprite.width;
                        b.in_bg.block_h = sprite.height;
                        b.in_bg.block_offset_x = sprite.x;
                        b.in_bg.block_offset_y = sprite.y - first;
                        b.in_bg.blend_cm = PPA_BLEND_COLOR_MODE_RGB888;
                        b.in_fg.buffer = _sprite;
                        b.in_fg.pic_w = SpriteSize;
                        b.in_fg.pic_h = SpriteSize;
                        b.in_fg.block_w = sprite.width;
                        b.in_fg.block_h = sprite.height;
                        b.in_fg.block_offset_x = sprite.x - _spriteX;
                        b.in_fg.block_offset_y = sprite.y - _spriteY;
                        b.in_fg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;
                        b.out.buffer = dst;
                        b.out.buffer_size = static_cast<uint32_t>(stride * count);
                        b.out.pic_w = _width;
                        b.out.pic_h = count;
                        b.out.block_offset_x = sprite.x;
                        b.out.block_offset_y = sprite.y - first;
                        b.out.blend_cm = PPA_BLEND_COLOR_MODE_RGB888;
                        b.bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
                        b.bg_alpha_fix_val = 255;
                        b.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
                        b.fg_alpha_fix_val = 255;
                        b.fg_ck_en = true;
                        b.fg_ck_rgb_low_thres.r = 0xf0;
                        b.fg_ck_rgb_low_thres.b = 0xf0;
                        b.fg_ck_rgb_high_thres.r = 0xff;
                        b.fg_ck_rgb_high_thres.g = 0x0f;
                        b.fg_ck_rgb_high_thres.b = 0xff;
                        b.mode = PPA_TRANS_MODE_BLOCKING;
                        if (ppa_do_blend(static_cast<ppa_client_handle_t>(_blendClient), &b) != ESP_OK) ++_stats.errors;
                        now = cycles();
                        account(_stats.blendCycles, _stats.blendMax, now - mark);
                }
        }

        ++_stats.blocks;
        _stats.pieces += static_cast<uint32_t>(pieces);
        if (pieces > _stats.piecesMax) _stats.piecesMax = static_cast<uint32_t>(pieces);
        _stats.jobs += jobs;
        account(_stats.blockCycles, _stats.blockMax, cycles() - start);
        if (_checkWanted.exchange(false)) runCheck(first, count, dst, stride);
        return;
}

// What (@p x, @p y) should show (the sprite left out), as 0xRRGGBB; bit 24
// set when it came from RGB565 (only the top bits are compared).
uint32_t ScanCompositor::expectedAt(uint16_t x, uint16_t y) const {
        for (size_t i = _layerCount; i-- > 0;) {
                const Layer &l = _layers[i];
                const int    lx = x - l.x, ly = y - l.y;
                if (lx < 0 || ly < 0 || lx >= l.width || ly >= l.height) continue;
                const uint8_t *p = l.data + (size_t(ly) * l.width + lx) * (l.format == Format::Rgb565 ? 2 : 3);
                if (l.format == Format::Rgb565) return rgb888(uint16_t(p[0] | p[1] << 8)) | 1u << 24;
                return uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
        }
        if (_background == Background::Fill) return _desktopColor;
        return rgb888(rgb565(_desktopColor)) | 1u << 24;
}

void ScanCompositor::runCheck(uint16_t first, uint16_t count, const uint8_t *dst, size_t stride) {
        (void)esp_cache_msync(const_cast<uint8_t *>(dst), stride * count, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        Check c;
        c.first = first;
        for (uint16_t r = 0; r < count; ++r) {
                const uint16_t y = static_cast<uint16_t>(first + r);
                for (uint16_t x = 0; x < _width; ++x) {
                        if (_spriteShown && x >= _spriteX && x < _spriteX + SpriteSize && y >= _spriteY &&
                            y < _spriteY + SpriteSize)
                                continue;
                        const uint8_t *p = dst + size_t(r) * stride + size_t(x) * 3;
                        const uint32_t got = uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
                        const uint32_t want = expectedAt(x, y);
                        const uint32_t mask = (want >> 24) != 0 ? 0xf8fcf8 : 0xffffff;
                        ++c.checked;
                        if ((got & mask) == (want & mask)) continue;
                        if (c.wrong++ == 0) {
                                c.x = x;
                                c.y = y;
                                c.got = got;
                                c.expected = want & 0xffffff;
                        }
                }
        }
        c.done = true;
        _checkResult = c;
        return;
}

BMDS_NAMESPACE_END
