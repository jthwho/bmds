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
 * A surface shorter than the mode is scanned with its rows repeated
 * (`devplan/games.md` §10): a descriptor per line of the mode, each reading
 * the surface's row that line falls on (row = line × rows / lines), so a
 * picture scaled up vertically costs only its own rows' memory and writes.
 * Its interrupts stay about `MaxBlocks` a frame. `show()` changes the
 * surface scanned, at a restart of the stream (a frame's top).
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
                                uint32_t laneMbps =
                                        0; ///< The lanes' rate, at least the pixels' (0: `VideoModes::laneMbps()`).
                                bool continuousClock = false; ///< The clock lane always in high speed.
                                bool frameAck = true;         ///< The host asks for an acknowledge after each frame.
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
                                uint32_t rebuilds = 0;       ///< Rebuilds of the whole DSI path (a stuck stream's).
                                uint32_t failures = 0;       ///< Restarts that failed.
                                uint32_t pixelHz = 0;        ///< The pixel clock as set.
                                uint32_t isrCyclesMax = 0;   ///< The slowest block interrupt.
                                uint32_t dmaItem = 0;        ///< The block the DMA is on.
                                uint32_t bridgeFifo = 0;     ///< The DSI bridge's FIFO depth.
                                uint32_t isrCycles = 0;      ///< The block interrupts' cycles in all (wraps).
                                uint32_t fallbacks = 0;      ///< Surfaces given up for stalling (`show()`).
                                uint32_t switches = 0;       ///< Surfaces (or modes) switched to (`show()`).
                                uint32_t linkErrorFrames = 0; ///< Frames in which the DSI host flagged an error.
                                uint32_t linkErrors0 = 0;     ///< The DSI host's INT_ST0 flags seen since `start()`.
                                uint32_t linkErrors1 = 0;     ///< Its INT_ST1 flags seen since `start()`.
                                uint32_t laneMbps = 0;        ///< The lanes' rate as set.
                                uint32_t restarts = 0;        ///< Switches (`show()`) made with the link kept up.
                };

                /** @brief A scanout on DSI bus @p bus (0 on the ESP32-P4). */
                explicit Scanout(int bus) : _bus(bus) {}
                ~Scanout() { stop(); }
                Scanout(const Scanout &) = delete;
                Scanout &operator=(const Scanout &) = delete;

                /**
                 * @brief Powers the PHY, sets up the link and the DPI timing, and
                 *        starts sending @p surface (started, the mode's width, at
                 *        most its height).
                 * @return `Ok`, `Invalid` (the config, or a size the blocks don't
                 *         divide), `NoMem`, `InvalidState` (running), or the
                 *         driver's error.
                 * @blocking
                 */
                Error start(const Config &config, Surface &surface);

                /** @brief Stops sending and frees everything. Idempotent. */
                void stop();

                /**
                 * @brief Sends @p surface (started, the mode's width, at most its
                 *        height) from the next restart of the stream, made now (the
                 *        picture skips a frame). The surface sent before goes on
                 *        being drawn; its presents swap at once from then on.
                 * The stream restarts on it at a frame's top with the link kept up
                 * (the link is rebuilt only if the DMA won't stop). A
                 * surface that stalls the stream in its first second is given up:
                 * the surface before comes back (`Stats::fallbacks`).
                 * @return `Ok`, `InvalidState` (not running), `Invalid` (the size),
                 *         `NoMem` (its descriptors), `Timeout`, or the link's error.
                 * @blocking
                 */
                Error show(Surface &surface, unsigned int timeoutMs);

                /**
                 * @brief As `show()`, at @p timing (fitted to the clocks already, the surface its
                 *        width and at most its height) over @p lanes data lanes (0: as now): the
                 *        link rebuilt at its clock, the monitor resyncing (at the timing and lanes
                 *        sent now, as `show(surface, timeoutMs)`: the link kept up). A surface that stalls
                 *        falls back with the timing and lanes it had.
                 * @blocking
                 */
                Error show(Surface &surface, const VideoTiming &timing, uint8_t lanes, unsigned int timeoutMs);

                /** @brief Returns the timing being sent (the supervisor's task, or while stopped). */
                const VideoTiming &timing() const { return _config.timing; }

                /** @brief Returns the data lanes in use (the supervisor's task, or while stopped). */
                uint8_t lanes() const { return _config.lanes; }

                /**
                 * @brief Sets how often a line-mode descriptor interrupts: one in @p every
                 *        (0: every one, the default: fewer stopped swaps, games.md §14). Taken
                 *        at the next `show()`. A tuning aid (`display rows`).
                 */
                void setLineInterrupts(uint32_t every) {
                        _lineInterrupts = every;
                        return;
                }

                /**
                 * @brief Sets the clock lane's mode and the frame acknowledge, at once and for
                 *        rebuilds (a test aid: `display link`).
                 * @par Thread Safety
                 * Not with `start()` or `stop()`.
                 */
                void setLinkOptions(bool continuousClock, bool frameAck);

                /**
                 * @brief Sets the lanes' rate (`Config::laneMbps`), taken when the link is next
                 *        built (`show()` with a timing builds it anew). A test aid (`display link`).
                 * @par Thread Safety
                 * Not with `start()` or `stop()`.
                 */
                void setLaneMbps(uint32_t mbps) {
                        _config.laneMbps = mbps;
                        return;
                }

                /**
                 * @brief Returns the DSI host's register block while the link is up, else null
                 *        (its clock is off: a read faults). A test aid (`display dsi`).
                 * @par Thread Safety
                 * Not with `start()` or `stop()`; a rebuild of the link (the supervisor's) can
                 * take the block away under a reader.
                 */
                volatile uint32_t *hostRegisters() const;

                /** @brief As `hostRegisters()`, the DSI bridge's (the DMA's side, to DPI). */
                volatile uint32_t *bridgeRegisters() const;

                /** @brief Returns the link options (`setLinkOptions()`). */
                const Config &config() const { return _config; }

                /** @brief Returns the surface being sent, or null. */
                Surface *surface() const { return _surface; }

                /** @brief Returns true while sending. */
                bool isRunning() const { return _running; }

                /** @brief Returns the counters. */
                Stats stats() const;

                /**
                 * @brief Holds the DMA (suspended at a burst's end) for @p us microseconds, so the
                 *        bridge runs dry: an underrun on purpose, to test the resync
                 *        (`display underrun`).
                 * @return `Ok`, or `InvalidState` if not sending.
                 * @par Thread Safety
                 * Not with `start()` or `stop()`; the supervisor may rebuild the link meanwhile.
                 */
                Error starve(unsigned int us);

        private:
                struct Hardware; // the IDF handles
                struct ItemList; // the DMA's descriptors

                // The descriptors' plan for a surface (planFor()): what the interrupt reads, so it
                // changes only while no link runs (apply() in rebuild(), after stopHardware()).
                struct Plan {
                                size_t   blockBytes = 0;
                                size_t   stride = 0;
                                uint32_t items = 0;
                                uint32_t rows = 0;
                                uint32_t irqEvery = 1;
                                bool     lineMode = false;
                                VideoTiming timing;
                                uint8_t     lanes = 0;
                };

                static void supervisorTask(void *arg);
                void        supervise();
                Error       startLink();
                Error       startDpi();
                void        configureBridge();
                Error       planFor(const Surface &surface, const VideoTiming &timing,
                                    Plan &plan) const; // the descriptors' plan for @p surface at @p timing
                void        apply(const Plan &plan);
                Error       layOut(const Surface &surface); // planFor() and apply(): the link down only
                Error       buildList();                    // the DMA's list for the plan, on the shown buffer
                void        useList();                      // the channel pointed at it
                void        switchSurface();                // show()'s request, on the supervisor's task
                bool        haltAtFrameEnd(); // the DMA stopped by itself at the frame's end (false: it didn't)
                bool        stopStream();     // the DPI output and the DMA stopped (false: the DMA won't)
                void        startStream();    // and started again at the frame's top
                void        resync(bool atFrameEnd);
                Error       rebuild(bool recovery); // the link anew (@p recovery: a stuck one's, counted)
                void        fallBack();
                void        stopHardware();

                int                    _bus;
                Config                 _config;
                Surface               *_surface = nullptr;
                Hardware              *_hw = nullptr;
                ItemList              *_prepared = nullptr;    // switchSurface()'s list, made before the link came down
                Surface               *_nextSurface = nullptr; // rebuild()'s to take once the link is down
                Plan                   _nextPlan;              // and its plan
                size_t                 _blockBytes = 0;        // an item's
                size_t                 _stride = 0;            // a row's bytes
                uint32_t               _items = 0;             // descriptors a frame
                uint32_t               _rows = 0;              // the surface's rows
                uint32_t               _irqEvery = 1;          // an item in this many interrupts (and the last two)
                bool                   _lineMode = false;      // an item a line, rows repeated
                uint32_t               _lastItem = 0;          // the descriptor the DMA was on at the last interrupt
                bool                   _primed = false;        // that is known
                uint8_t                _swapPhase = 0;         // 1: all but the last descriptor on the new buffer
                bool                   _notifyPending = false; // the surface's waiters to wake (the cache was off)
                bool                   _running = false;
                std::atomic<bool>      _stopping{false};
                std::atomic<uint32_t>  _sent{0};   // blocks sent since the last restart
                std::atomic<uint32_t>  _frames{0}; // frames begun
                std::atomic<uint32_t>  _swaps{0};
                std::atomic<uint32_t>  _stalls{0};
                std::atomic<uint32_t>  _underruns{0};
                std::atomic<bool>      _stalled{false};
                std::atomic<bool>      _halt{false};     // haltAtFrameEnd() asks the interrupt
                std::atomic<bool>      _halted{false};   // and the DMA stopped where asked
                std::atomic<bool>      _underran{false}; // the bridge ran dry: the picture is out of step
                uint32_t               _isrCyclesMax = 0;
                uint32_t               _isrCycles = 0; // written by the interrupt alone
                uint32_t               _linkErrorFrames = 0; // the DSI host's flags, read a frame (the interrupt's)
                uint32_t               _linkErrors0 = 0;
                uint32_t               _linkErrors1 = 0;
                uint32_t               _resyncs = 0;
                uint32_t               _rebuilds = 0;
                uint32_t               _switches = 0;
                uint32_t               _restarts = 0; // the switches made with the link up
                uint32_t               _failures = 0;
                uint32_t               _blocksBefore = 0; // blocks sent before the last restart
                Semaphore              _wake{4, 0};
                Semaphore              _switched{1, 0};
                std::atomic<Surface *> _request{nullptr};   // show()'s
                VideoTiming            _requestTiming;      // and its timing (written before _request)
                uint8_t                _requestLanes = 0;   // and its lanes (0: as now)
                bool                   _requestHasTiming = false;
                Surface               *_fallback = nullptr; // the surface before a show(), while on probation
                VideoTiming            _fallbackTiming;     // and its timing
                uint8_t                _fallbackLanes = 0;  // and lanes
                uint32_t               _fallbackStalls = 0; // the stalls when it was shown
                int64_t                _fallbackUs = 0;
                uint32_t               _fallbacks = 0;
                uint32_t               _lineInterrupts = 0;
                Error                  _switchResult;
                Task                   _task;

                friend struct ScanoutIsr;
};

BMDS_NAMESPACE_END
