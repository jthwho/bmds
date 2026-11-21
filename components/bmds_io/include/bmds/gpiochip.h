/**
 * @file      gpiochip.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/stream.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An open `/dev/gpiochip0` (devplan/io.md §3.9, §12.3): what `gpio_ctl` operates on
 *        (the loader's), and Linux's GPIO character device through `linux.c`. Its reads come
 *        from `Io` (<bmds/io.h>); the lines it watches tell it their changes, which reading
 *        it gives as Linux's `gpio_v2_line_info_changed` records.
 *
 * One per open, in PSRAM (Linux keeps watches per file); it frees itself with its last handle.
 *
 * @par Thread Safety
 * Any task.
 */
class GpioChip final : public Stream {
        public:
                /** @brief A line as Linux describes it (`gpio_v2_line_info`'s words). */
                struct LineInfo {
                                char     name[32] = {};     ///< The board's name for it.
                                char     consumer[32] = {}; ///< Who holds it, or "".
                                uint64_t flags = 0;         ///< `IoLineConfig`'s flags (Linux's), `Used` among them.
                                uint32_t debounceUs = 0;    ///< A request's debounce, or 0.
                                uint32_t offset = 0;        ///< The line.
                };

                /** @brief A change told (Linux's `GPIO_V2_LINE_CHANGED_...`). */
                static constexpr uint32_t Requested = 1;
                static constexpr uint32_t Released = 2;
                static constexpr uint32_t Reconfigured = 3;

                /** @brief Changes waiting to be read, at most (Linux's 32); more are dropped. */
                static constexpr size_t MaxEvents = 32;

                /** @brief Linux's `gpio_v2_line_info_changed`: what reading a watching fd gives. */
                struct Changed {
                                char     name[32];
                                char     consumer[32];
                                uint32_t offset;
                                uint32_t numAttrs;
                                uint64_t flags;
                                struct {
                                                uint32_t id;
                                                uint32_t padding;
                                                uint64_t value;
                                } attrs[10];
                                uint32_t infoPadding[4];
                                uint64_t timestampNs;
                                uint32_t eventType; ///< `Requested`, `Released`, `Reconfigured`.
                                uint32_t padding[5];
                };
                static_assert(sizeof(Changed) == 288, "Linux's gpio_v2_line_info_changed");

                /**
                 * @brief Changes to lines made as one: watchers are told once, when the last
                 *        `Change` alive goes. Made before any lock its maker takes (telling reads
                 *        the pads, which takes `io/gpio`'s and the registry's).
                 *
                 * @par Thread Safety
                 * Any task.
                 */
                class Change {
                        public:
                                Change();
                                ~Change();
                                Change(const Change &) = delete;
                                Change &operator=(const Change &) = delete;
                };

                /** @brief Adds `/dev/gpiochip0`. After `Io::init()` and the VFS; once. */
                static Error addFiles();

                /**
                 * @brief Line @p offset as Linux describes it: the pad's direction, drive and
                 *        bias, or a program's request's settings (active low, edges, debounce, the
                 *        clock) for a line it holds.
                 * @return `Ok`, `Invalid` (off the chip).
                 * @blocking
                 */
                static Error lineInfo(unsigned offset, LineInfo &out);

                /**
                 * @brief Lines changed (a claim, a release, a pad set up again): every watching fd
                 *        compares its lines and queues what moved, or, inside a `Change`, at its end.
                 * @blocking Called with no lock of the registry's or `io/gpio`'s held.
                 */
                static void linesChanged();

                /**
                 * @brief Watches line @p offset (Linux's `GPIO_V2_GET_LINEINFO_WATCH_IOCTL`): its
                 *        info into @p out, its changes queued from now.
                 * @return `Ok`; `Invalid` (off the chip), `Busy` (watched already), `NoMem`, `Full`
                 *         (too many fds watch).
                 * @blocking
                 */
                Error watch(unsigned offset, LineInfo &out);

                /** @brief Stops watching line @p offset. @return `Ok`, `Invalid`, `Busy` (not watched). @blocking */
                Error unwatch(unsigned offset);

                /** @brief Returns N of `/dev/gpiochipN`. */
                uint32_t index() const { return _index; }

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan, unsigned int) override { return makeError<size_t>(Error::Invalid); }
                uint8_t        readiness() override;
                WaitList      *readinessWaiters() override;
                GpioChip      *gpioChip() override { return this; }
                void           handleClosed(uint8_t mode) override;

                struct Watch; // its watches and their queue (gpiochip.cpp), made at the first

        private:
                explicit GpioChip(uint32_t index) : _index(index) {}
                ~GpioChip() = default;

                Watch *watchState();
                void   compare();

                uint32_t _index;
                Watch   *_watch = nullptr;
};

BMDS_NAMESPACE_END
