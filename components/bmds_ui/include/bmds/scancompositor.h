/**
 * @file      scancompositor.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/blitchain.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/rect.h>
#include <bmds/region.h>
#include <bmds/result.h>
#include <bmds/scanout.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The scan-time composition prototype (`devplan/windows.md` §4b):
 *        test windows (layers) composed block by block straight into the
 *        scanout's ring, with their positions taken once a frame.
 *
 * Each block's visible pieces come from `planCompose()`; each piece is a
 * 2D-DMA copy (RGB565 layers widened, RGB888 ones copied), one job a piece
 * or chained (a job a format a block). The desktop is a surface (its pieces
 * copied) or a PPA fill of the whole block under the windows. A sprite with
 * a color key can be blended on top by the PPA.
 *
 * @par Thread Safety
 * Layers are made before the scan starts; positions and modes change from
 * any task (taken at the next frame's start); the rest runs on the
 * scanout's refill task.
 */
class ScanCompositor final : public BlockSource {
        public:
                /** @brief Layers at most. */
                static constexpr size_t MaxLayers = 8;

                /** @brief Pieces a block at most. */
                static constexpr size_t MaxPieces = 64;

                /** @brief The sprite's side. */
                static constexpr uint16_t SpriteSize = 32;

                /** @brief A layer's pixels. */
                enum class Format : uint8_t {
                        Rgb565,
                        Rgb888,
                        // Indexed8 (a palette per layer, through the PPA's CLUT) is planned.
                };

                /** @brief Under the windows. */
                enum class Background : uint8_t {
                        Surface, ///< A full-screen RGB565 surface, its pieces copied.
                        Fill,    ///< A PPA fill of each whole block, the windows copied over it.
                };

                /** @brief How the pieces go to the 2D-DMA. */
                enum class Jobs : uint8_t {
                        Each,    ///< A job a piece.
                        Chained, ///< A job a format a block.
                };

                /** @brief Counts since the last reset. */
                struct Stats {
                                uint32_t blocks = 0;
                                uint32_t pieces = 0;
                                uint32_t piecesMax = 0; ///< The most in one block.
                                uint32_t jobs = 0;
                                uint32_t planFull = 0; ///< Blocks left stale: too many pieces.
                                uint32_t errors = 0;   ///< DMA or PPA errors.
                                uint64_t planCycles = 0, copyCycles = 0, fillCycles = 0, blendCycles = 0;
                                uint32_t planMax = 0, copyMax = 0, fillMax = 0, blendMax = 0, blockMax = 0;
                                uint64_t blockCycles = 0;
                };

                /** @brief What `check()` found in one block. */
                struct Check {
                                bool     done = false;
                                uint16_t first = 0; ///< The block's first line.
                                uint32_t checked = 0;
                                uint32_t wrong = 0;
                                uint16_t x = 0, y = 0;          ///< The first wrong pixel.
                                uint32_t got = 0, expected = 0; ///< As 0xRRGGBB.
                };

                ScanCompositor() = default;
                ~ScanCompositor() { stop(); }
                ScanCompositor(const ScanCompositor &) = delete;
                ScanCompositor &operator=(const ScanCompositor &) = delete;

                /**
                 * @brief Allocates the desktop surface, the sprite, the 2D-DMA
                 *        chains and the PPA clients for a @p width × @p height
                 *        screen.
                 * @return `Ok`, `InvalidState`, `Invalid` (an odd width), `NoMem`,
                 *         or a driver's error.
                 */
                Error start(uint16_t width, uint16_t height, uint32_t desktopColor);

                /** @brief Frees everything (the scan stopped first). Idempotent. */
                void stop();

                /**
                 * @brief Adds a @p width × @p height layer on top at (@p x, @p y),
                 *        drawn with test pattern @p pattern. Before the scan starts.
                 * @return The layer's index, `InvalidState`, `Full`, `Invalid`, `NoMem`.
                 */
                Result<uint8_t> addLayer(Format format, uint16_t width, uint16_t height, int x, int y, uint8_t pattern);

                /** @brief Returns the layers. */
                size_t layerCount() const { return _layerCount; }

                /** @brief Moves layer @p index (x rounded to even) from the next frame. */
                void moveLayer(size_t index, int x, int y);

                /** @brief Moves layer @p index by (@p dx, @p dy) every frame, bouncing; (0, 0) stops. */
                void dragLayer(size_t index, int dx, int dy);

                /** @brief Shows the sprite at (@p x, @p y), or hides it. */
                void setSprite(bool shown, int x, int y);

                /** @brief Sets the background from the next frame. */
                void setBackground(Background background) {
                        _wantBackground.store(background);
                        return;
                }

                /** @brief Sets how pieces go to the DMA from the next frame. */
                void setJobs(Jobs jobs) {
                        _wantJobs.store(jobs);
                        return;
                }

                /** @brief Returns the counts. */
                Stats stats() const { return _stats; }

                /** @brief Clears the counts (from the next block). */
                void resetStats() {
                        _resetWanted.store(true);
                        return;
                }

                /** @brief Checks the next block against what it should show; `checkResult()` has it. */
                void check() {
                        _checkResult.done = false;
                        _checkWanted.store(true);
                        return;
                }

                /** @brief Returns the last check's result. */
                Check checkResult() const { return _checkResult; }

                // BlockSource
                void beginFrame(uint32_t frame) override;
                void composeBlock(uint16_t first, uint16_t count, uint8_t *dst, size_t stride) override;

        private:
                struct Layer {
                                uint8_t              *data = nullptr;
                                uint16_t              width = 0;
                                uint16_t              height = 0;
                                Format                format = Format::Rgb565;
                                std::atomic<uint32_t> wanted{0};    // x, y as two int16
                                int16_t               x = 0, y = 0; // as taken for the frame
                                int16_t               dx = 0, dy = 0;
                };

                static uint32_t pack(int x, int y) { return uint32_t(uint16_t(x)) << 16 | uint16_t(y); }
                Rect            onScreen(int x, int y, uint16_t width, uint16_t height) const;
                uint32_t        expectedAt(uint16_t x, uint16_t y) const;
                void            runCheck(uint16_t first, uint16_t count, const uint8_t *dst, size_t stride);

                uint16_t                _width = 0;
                uint16_t                _height = 0;
                uint32_t                _desktopColor = 0;
                uint8_t                *_desktop = nullptr; // RGB565, full screen
                uint8_t                *_sprite = nullptr;  // RGB565, SpriteSize square, magenta = clear
                Layer                   _layers[MaxLayers];
                size_t                  _layerCount = 0;
                BlitChain               _chain565;
                BlitChain               _chain888;
                void                   *_fillClient = nullptr; // ppa_client_handle_t
                void                   *_blendClient = nullptr;
                ComposePiece            _pieces[MaxPieces];
                std::atomic<Background> _wantBackground{Background::Surface};
                std::atomic<Jobs>       _wantJobs{Jobs::Chained};
                std::atomic<uint32_t>   _wantSprite{0}; // bit 31: shown; then x, y as 15 bits each
                Background              _background = Background::Surface;
                Jobs                    _jobs = Jobs::Chained;
                bool                    _spriteShown = false;
                int16_t                 _spriteX = 0, _spriteY = 0;
                std::atomic<bool>       _resetWanted{false};
                std::atomic<bool>       _checkWanted{false};
                Check                   _checkResult;
                Stats                   _stats;
};

BMDS_NAMESPACE_END
