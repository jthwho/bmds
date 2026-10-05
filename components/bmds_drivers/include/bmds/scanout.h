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
#include <bmds/edid.h>
#include <bmds/error.h>
#include <bmds/semaphore.h>
#include <bmds/surface.h>
#include <bmds/task.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A surface on the MIPI-DSI output (`devplan/display.md` §5c): the
 *        DMA reads its shown buffer straight from PSRAM into the DSI bridge.
 *
 * The DW-GDMA streams a circular list of one descriptor a block (the frame
 * in up to `MaxBlocks` blocks); each block's interrupt makes the finished
 * descriptors valid again (the DMA's write-back clears them) and counts the
 * frames. A double-buffered surface's swap is made at a frame's start: when
 * one is asked for and the DMA is on the frame's last block, the other
 * descriptors are pointed at the new buffer, and at the next frame's start
 * the last one too.
 *
 * A supervisor task restarts the stream if the DMA ever meets an invalid
 * descriptor or stops moving.
 *
 * One scanout per DSI bus. The bridge chip (an HDMI bridge, a panel) is set
 * up by its own driver, before `start()` (it expects the video to follow).
 *
 * @par Thread Safety
 * `start()` and `stop()` from one task; `stats()` from any.
 */
class Scanout {
        public:
                /** @brief Most blocks a frame. */
                static constexpr uint32_t MaxBlocks = 16;

                /** @brief What to send. */
                struct Config {
                                VideoTiming timing;       ///< The mode (its pixel clock sets the lane rate).
                                uint8_t     lanes = 2;    ///< DSI data lanes.
                                int8_t      phyLdo = -1;  ///< On-chip LDO for the DSI PHY, or -1.
                                uint16_t    phyLdoMv = 0; ///< Its voltage.
                                int         core = 1;     ///< The supervisor task's core.
                                void (*onRestart)(void *arg) =
                                        nullptr;              ///< After a rebuild of the link: re-lock the bridge chip.
                                void *onRestartArg = nullptr; ///< Its argument.
                };

                /** @brief Counters since `start()`. */
                struct Stats {
                                uint32_t frames = 0;         ///< Frames begun.
                                uint32_t blocks = 0;         ///< Blocks sent.
                                uint32_t blocksPerFrame = 0; ///< The frame's blocks.
                                uint32_t swaps = 0;          ///< Double-buffer swaps made.
                                uint32_t stalls = 0;         ///< The DMA met an invalid descriptor.
                                uint32_t underruns = 0;      ///< The DSI bridge ran dry.
                                uint32_t resyncs = 0;        ///< Restarts of the stream.
                                uint32_t rebuilds = 0;       ///< Rebuilds of the whole DSI path.
                                uint32_t failures = 0;       ///< Restarts that failed.
                                uint32_t pixelHz = 0;        ///< The pixel clock as set.
                                uint32_t isrCyclesMax = 0;   ///< The slowest block interrupt.
                                uint32_t dmaItem = 0;        ///< The block the DMA is on.
                                uint32_t bridgeFifo = 0;     ///< The DSI bridge's FIFO depth.
                };

                /** @brief A scanout on DSI bus @p bus (0 on the ESP32-P4). */
                explicit Scanout(int bus) : _bus(bus) {}
                ~Scanout() { stop(); }
                Scanout(const Scanout &) = delete;
                Scanout &operator=(const Scanout &) = delete;

                /**
                 * @brief Powers the PHY, sets up the link and the DPI timing, and
                 *        starts sending @p surface (started, the mode's size).
                 * @return `Ok`, `Invalid` (the config, or a size the blocks don't
                 *         divide), `NoMem`, `InvalidState` (running), or the
                 *         driver's error.
                 * @blocking
                 */
                Error start(const Config &config, Surface &surface);

                /** @brief Stops sending and frees everything. Idempotent. */
                void stop();

                /** @brief Returns true while sending. */
                bool isRunning() const { return _running; }

                /** @brief Returns the counters. */
                Stats stats() const;

        private:
                struct Hardware; // the IDF handles

                static void supervisorTask(void *arg);
                void        supervise();
                Error       startLink();
                Error       startDpi();
                void        configureBridge();
                void        resync();
                void        rebuild();
                void        stopHardware();

                int                   _bus;
                Config                _config;
                Surface              *_surface = nullptr;
                Hardware             *_hw = nullptr;
                size_t                _blockBytes = 0;
                uint32_t              _blocks = 0;     // a frame's
                uint32_t              _lastItem = 0;   // the descriptor the DMA was on at the last interrupt
                bool                  _primed = false; // that is known
                uint8_t               _swapPhase = 0;  // 1: all but the last descriptor on the new buffer
                bool                  _running = false;
                std::atomic<bool>     _stopping{false};
                std::atomic<uint32_t> _sent{0};   // blocks sent since the last restart
                std::atomic<uint32_t> _frames{0}; // frames begun
                std::atomic<uint32_t> _swaps{0};
                std::atomic<uint32_t> _stalls{0};
                std::atomic<uint32_t> _underruns{0};
                std::atomic<bool>     _stalled{false};
                uint32_t              _isrCyclesMax = 0;
                uint32_t              _resyncs = 0;
                uint32_t              _rebuilds = 0;
                uint32_t              _failures = 0;
                uint32_t              _blocksBefore = 0; // blocks sent before the last restart
                Semaphore             _wake{4, 0};
                Task                  _task;

                friend struct ScanoutIsr;
};

BMDS_NAMESPACE_END
