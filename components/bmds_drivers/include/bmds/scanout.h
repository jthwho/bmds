/**
 * @file      scanout.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/colorexpander.h>
#include <bmds/edid.h>
#include <bmds/error.h>
#include <bmds/paletteexpander.h>
#include <bmds/semaphore.h>
#include <bmds/task.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Draws lines of the picture as they are about to be sent
 *        (`Scanout`).
 *
 * Called from the scanout's refill task, never from an interrupt: it must
 * finish well within the time the lines take to send (at 1280×720, 60 Hz,
 * 22.5 µs a line), allocate nothing and not block.
 */
class LineSource {
        public:
                /**
                 * @brief Fills @p count lines starting at line @p first (0 is the
                 *        top) into @p dst, @p stride bytes apart, in the scanout's
                 *        format and width.
                 * @hotpath
                 */
                virtual void render(uint16_t first, uint16_t count, uint8_t *dst, size_t stride) = 0;

                /**
                 * @brief A framebuffer scanout (`Config::framebuffer`) started a
                 *        frame: change what moves in @p frame (the whole picture,
                 *        @p stride bytes a line), and write back from the cache
                 *        what it changed (a write-back of the whole frame would
                 *        hold up the DMA's reads).
                 */
                virtual void frame(uint8_t *frame, size_t stride) {
                        (void)frame;
                        (void)stride;
                        return;
                }

        protected:
                ~LineSource() = default;
};

/**
 * @brief A line-rendered picture on the MIPI-DSI output (`devplan/display.md`
 *        §5): no framebuffer.
 *
 * The DSI controller's DMA streams a circular ring of `blocks` buffers of
 * `linesPerBlock` lines each, in internal DMA RAM. When a buffer has been
 * sent, the refill task (pinned to a core, high priority) asks the
 * `LineSource` for the lines that buffer carries next and hands it back to
 * the DMA. The DSI bridge widens 16-bit lines to the link's 24 bits.
 *
 * One scanout per DSI bus. The bridge chip (an HDMI bridge, a panel) is set
 * up by its own driver, before `start()` (it expects the video to follow).
 *
 * @par Thread Safety
 * `start()` and `stop()` from one task; `stats()` from any.
 */
class Scanout {
        public:
                /** @brief Line buffer pixel formats. */
                enum class Format : uint8_t {
                        Rgb565,   ///< 16 bits: R5 G6 B5, little-endian.
                        Rgb888,   ///< 24 bits: B, G, R bytes.
                        Indexed8, ///< 8-bit palette indices, expanded to RGB888 by the PPA (`palette()`).
                        Rgb565Expanded, ///< Drawn as RGB565, widened to RGB888 by the 2D-DMA.
                };

                /** @brief What to send. */
                struct Config {
                                VideoTiming timing;                  ///< The mode (its pixel clock sets the lane rate).
                                Format      format = Format::Rgb565; ///< Line buffer format.
                                uint8_t     lanes = 2;               ///< DSI data lanes.
                                uint16_t    linesPerBlock = 8;       ///< Lines per buffer; divides `timing.vActive`.
                                uint8_t     blocks = 3;              ///< Buffers in the ring (2 or more).
                                int8_t      phyLdo = -1;             ///< On-chip LDO for the DSI PHY, or -1.
                                uint16_t    phyLdoMv = 0;            ///< Its voltage.
                                int         core = 1;                ///< The refill task's core.
                                bool        framebuffer =
                                        false; ///< The ring is a whole frame in PSRAM (RGB888): drawn once, not per block.
                                uint8_t linkPacking =
                                        0; ///< 16/18-bit links: the host's packing variant (0-2), the bridge's arrangement.
                                bool            fullLaneRate = false; ///< Lanes at the 24-bit rate whatever the format.
                                bool            freeRunning =
                                        false; ///< The ring never stops: a late block goes out stale and is skipped.
                                const uint32_t *palette =
                                        nullptr; ///< `Indexed8`: 256 entries (0xRRGGBB) loaded at the start.
                                void (*onRestart)(void *arg) =
                                        nullptr;              ///< After a rebuild of the link: re-lock the bridge chip.
                                void *onRestartArg = nullptr; ///< Its argument.
                };

                /** @brief Counters since `start()`. */
                struct Stats {
                                uint32_t blocks = 0;    ///< Buffers sent.
                                uint32_t frames = 0;    ///< Whole frames sent.
                                uint32_t late = 0;      ///< Buffers sent again unrefilled (the renderer fell behind).
                                uint32_t skipped = 0;   ///< `freeRunning`: blocks not drawn in time, sent stale.
                                uint32_t stalls = 0;    ///< The DMA found a buffer not handed back yet.
                                uint32_t underruns = 0; ///< The DSI bridge ran dry.
                                uint32_t resyncs = 0;   ///< Restarts of the data path after an underrun or a stall.
                                uint32_t resyncFailures = 0;  ///< Rebuilds that failed.
                                uint32_t rebuilds = 0;        ///< Rebuilds of the whole DSI path (a stuck stream).
                                uint32_t pixelHz = 0;         ///< The pixel clock as set.
                                uint32_t renderCyclesMax = 0; ///< The slowest refill, CPU cycles.
                                uint64_t renderCycles = 0;    ///< All refills, CPU cycles.
                                uint64_t syncCycles = 0;      ///< Of which writing the buffers back from the cache.
                                uint32_t syncCyclesMax = 0;   ///< The slowest write-back.
                                uint64_t expandCycles = 0;    ///< Of which waiting for the PPA or the 2D-DMA.
                                uint32_t expandCyclesMax = 0; ///< The slowest expansion.
                                uint32_t expandErrors = 0;    ///< Expansions that failed or timed out.
                                // Bring-up diagnostics.
                                uint32_t dmaItem = 0;     ///< The ring buffer the DMA is on.
                                uint32_t dmaStatus = 0;   ///< The DMA channel's raw event bits.
                                uint32_t dmaAmount = 0;   ///< Items moved in the current block.
                                uint32_t bridgeFifo = 0;  ///< The DSI bridge's FIFO depth.
                                uint32_t stallAt[8] = {}; ///< Where in the frame (block) the last stalls were.
                };

                /** @brief A scanout on DSI bus @p bus (0 on the ESP32-P4). */
                explicit Scanout(int bus) : _bus(bus) {}
                ~Scanout() { stop(); }
                Scanout(const Scanout &) = delete;
                Scanout &operator=(const Scanout &) = delete;

                /**
                 * @brief Powers the PHY, sets up the link and the DPI timing, fills
                 *        the ring and starts sending.
                 * @return `Ok`, `Invalid` (the config), `NoMem`, `InvalidState`
                 *         (running), or the driver's error.
                 * @blocking
                 */
                Error start(const Config &config, LineSource &source);

                /** @brief Stops sending and frees everything. Idempotent. */
                void stop();

                /** @brief Returns true while sending. */
                bool isRunning() const { return _running; }

                /** @brief Returns the counters. */
                Stats stats() const;

                /** @brief Returns the palette (`Indexed8`): set entries before `start()` or from the source. */
                PaletteExpander &palette() { return _expander; }

                /** @brief Bring-up: restarts the data path now; @p variant 2 also restarts the host's video mode. */
                void requestResync(int variant) {
                        _resyncVariant = variant;
                        _resyncWanted.store(true);
                        _resyncUs = 0;
                        _wake.release();
                        return;
                }

                /** @brief Returns the bytes of internal RAM the ring takes. */
                size_t ringBytes() const { return _blockBytes * _config.blocks; }

        private:
                struct Hardware; // the IDF handles

                static void refillTask(void *arg);
                void        refill();
                void        renderSlot(uint32_t block);
                Error       startLink();
                Error       startDpi();
                void        resync();
                void        rebuild();
                void        stopHardware();
                void        configureBridge();

                int                   _bus;
                Config                _config;
                LineSource           *_source = nullptr;
                Hardware             *_hw = nullptr;
                uint8_t              *_ring = nullptr;    // blocks × _blockBytes
                uint8_t              *_staging = nullptr; // Indexed8, Rgb565Expanded: one block as drawn
                PaletteExpander       _expander;
                ColorExpander         _widener;
                size_t                _blockBytes = 0;
                size_t                _stride = 0;
                uint32_t              _blocksPerFrame = 0;
                bool                  _running = false;
                std::atomic<bool>     _stopping{false};
                std::atomic<uint32_t> _sent{0};      // buffers sent (the DMA's block interrupts)
                uint32_t              _refilled = 0; // buffers handed back, counted like `_sent`
                std::atomic<uint32_t> _late{0};
                uint32_t              _skipped = 0;
                std::atomic<uint32_t> _stalls{0};
                std::atomic<uint32_t> _underruns{0};
                std::atomic<bool>     _stalled{false};
                std::atomic<bool>     _resyncWanted{false}; // an underrun or a stall: the picture is offset
                uint32_t              _resyncs = 0;
                uint32_t              _resyncFailures = 0;
                uint32_t              _rebuilds = 0;
                int                   _resyncVariant = 1;
                uint32_t              _blocksBefore = 0; // blocks sent before the last resync
                int64_t               _resyncUs = 0;     // when the last resync was
                uint32_t              _stallAt[8] = {};
                uint32_t              _renderCyclesMax = 0;
                uint64_t              _renderCycles = 0;
                uint64_t              _syncCycles = 0;
                uint32_t              _syncCyclesMax = 0;
                uint64_t              _expandCycles = 0;
                uint32_t              _expandCyclesMax = 0;
                uint32_t              _expandErrors = 0;
                Semaphore             _wake{64, 0};
                Task                  _task;

                friend struct ScanoutIsr;
};

BMDS_NAMESPACE_END
