/**
 * @file      patchsource.h
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
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Where the patchbay reads and changes the graph (`devplan/audio.md`
 *        §24): the ports, their connections, a watch on changes. The built-in's
 *        answers from the kernel's public classes (`KernelPatchSource`); one
 *        built apart would answer from the graph fd.
 * @par Thread Safety
 * The patchbay's loop, but `Changed` (any task).
 */
class PatchSource {
        public:
                /** @brief A port (its name with its NUL, its kind's name). */
                struct Port {
                                uint8_t number = 0;
                                bool    source = false;      ///< It has a source (what it plays).
                                bool    destination = false; ///< It has a destination (it can be played).
                                char    name[32] = {};
                                char    kind[8] = {};
                };

                /** @brief A connection, by port numbers. */
                struct Connection {
                                uint8_t source = 0;
                                uint8_t destination = 0;
                };

                /** @brief Told that the graph changed: on any task; must not block. */
                using Changed = void (*)(void *arg);

                virtual ~PatchSource() = default;

                /** @brief Copies the ports into @p out, by number. @return How many there are. */
                virtual size_t ports(Span<Port> out) = 0;

                /** @brief Copies the connections into @p out. @return How many there are. */
                virtual size_t connections(Span<Connection> out) = 0;

                /** @brief Connects @p source to @p destination, kept by name (a reboot, a replug). */
                virtual Error connect(uint8_t source, uint8_t destination) = 0;

                /** @brief Ends that connection, and its keeping. */
                virtual Error disconnect(uint8_t source, uint8_t destination) = 0;

                /** @brief Watches the graph: @p fn is told of each change until `unwatch()`. */
                virtual Error watch(Changed fn, void *arg) = 0;

                /** @brief Stops watching; once it returns, the watch isn't called. */
                virtual void unwatch() = 0;
};

/** @brief The built-in's source: the MIDI router, `Midi`'s watchers, `midi/connect`. */
class KernelPatchSource final : public PatchSource {
        public:
                KernelPatchSource() = default;
                ~KernelPatchSource() override { unwatch(); }

                size_t ports(Span<Port> out) override;
                size_t connections(Span<Connection> out) override;
                Error  connect(uint8_t source, uint8_t destination) override;
                Error  disconnect(uint8_t source, uint8_t destination) override;
                Error  watch(Changed fn, void *arg) override;
                void   unwatch() override;

        private:
                Error keep(uint8_t source, uint8_t destination, bool kept);

                Changed _fn = nullptr;
                void   *_arg = nullptr;
};

BMDS_NAMESPACE_END
