/**
 * @file      pads.h
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
#include <bmds/padmapping.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The gamepads attached, numbered (`pad0`, `pad1`...), each presented
 *        as the standard pad (`devplan/controls.md` §4, §7a).
 *
 * A driver attaches a pad and hands it each report; a report becomes events
 * only where something changed. Pads are numbered in the order connected;
 * a pad that comes back (the same vendor, product and hub port) takes the
 * number it had.
 *
 * @par Thread Safety
 * All functions from any task, except `init()`; the drivers' calls on their
 * own task.
 */
namespace Pads {

        /** @brief Pads at once. */
        inline constexpr size_t MaxPads = 8;

        /** @brief Bytes of a report descriptor kept (`pad descriptor`). */
        inline constexpr size_t MaxDescriptor = 512;

        /** @brief Bytes of a pad's name kept (ASCII, NUL included). */
        inline constexpr size_t MaxName = 32;

        /** @brief The mappings database in the ROM (SDL's format, `devplan/controls.md` §7a). */
        inline constexpr StringView RomDatabase("/rom/share/gamepads.txt");

        /** @brief How a pad speaks. */
        enum class Kind : uint8_t {
                Hid, ///< A HID joystick or game pad.
        };

        /** @brief Where a pad's mapping came from. */
        enum class Origin : uint8_t {
                Database, ///< Its line in the database.
                Standard, ///< The usual order: no line for it.
        };

        /** @brief What is known of an attached pad. */
        struct Info {
                        uint8_t  number = 0;
                        Kind     kind = Kind::Hid;
                        Origin   origin = Origin::Standard;
                        uint16_t vendor = 0;
                        uint16_t product = 0;
                        uint8_t  axes = 0;
                        uint8_t  hats = 0;
                        uint8_t  buttons = 0;
                        char     name[MaxName] = {}; ///< The device's product string; may be empty.
        };

        /** @brief A change, as a listener hears it. */
        struct Event {
                        /** @brief What changed. */
                        enum class Type : uint8_t {
                                Attached,
                                Detached,
                                RawAxis,   ///< `index`: the axis (`aN`); `value`: −32768 to 32767.
                                RawHat,    ///< `index`: the hat; `value`: its `RawPad::Hat` bits.
                                RawButton, ///< `index`: the button (`bN`); `value`: 1 down, 0 up.
                                Axis,      ///< `index`: a `PadState::Axis`; `value`: its value.
                                Button,    ///< `index`: a `PadState::Button`; `value`: 1 down, 0 up.
                        };
                        Type    type = Type::Attached;
                        uint8_t pad = 0;
                        uint8_t index = 0;
                        int32_t value = 0;
        };

        /**
         * @brief Hears every pad's events (`pad test`): a ring of them, and a semaphore
         *        released as they come. Events that don't fit are counted and dropped.
         */
        class Listener {
                public:
                        static constexpr size_t Capacity = 64;

                        Listener() = default;
                        Listener(const Listener &) = delete;
                        Listener &operator=(const Listener &) = delete;

                        /** @brief Takes the oldest event. @return False if there is none. @nonblocking */
                        bool take(Event &event);

                        /** @brief Returns the semaphore released when events come (wait on it). */
                        Semaphore &ready() { return _ready; }

                        /** @brief Returns how many events didn't fit. */
                        uint32_t dropped() const { return _dropped; }

                        /// @cond INTERNAL
                        void put(const Event &event);
                        /// @endcond

                private:
                        Semaphore _ready{1, 0};
                        Event     _events[Capacity];
                        uint32_t  _head = 0; // the next to take
                        uint32_t  _count = 0;
                        uint32_t  _dropped = 0;
        };

        /** @brief Boot stage: makes the table (PSRAM). Before any driver starts. */
        Error init();

        // The drivers'.

        /**
         * @brief Attaches a HID pad by its report descriptor; its mapping is looked up.
         * @param location Where it is plugged in (its hub's address and port), to know it again.
         * @return Its number; `NotSupported` if the descriptor has no joystick or game pad,
         *         `Full` past `MaxPads`.
         */
        Result<uint8_t> attachHid(uint16_t vendor, uint16_t product, uint16_t location, Span<const uint8_t> descriptor);

        /** @brief Sets pad @p number's name (its product string, when it arrives). */
        void setName(uint8_t number, StringView name);

        /** @brief Hands pad @p number a report, as received. @hotpath */
        void reportHid(uint8_t number, Span<const uint8_t> report);

        /** @brief Detaches pad @p number: its buttons go up. */
        void detach(uint8_t number);

        // Everyone's.

        /** @brief Copies the attached pads' details into @p out, by number. @return How many. */
        size_t list(Span<Info> out);

        /** @brief Copies pad @p number's details. @return `NotFound` if it isn't attached. */
        Error info(uint8_t number, Info &out);

        /** @brief Copies pad @p number's mapping. @return `NotFound` if it isn't attached. */
        Error mapping(uint8_t number, PadMapping &out);

        /**
         * @brief Copies pad @p number's report descriptor into @p out.
         * @return Its length (at most `MaxDescriptor`), or `NotFound`.
         */
        Result<size_t> descriptor(uint8_t number, Span<uint8_t> out);

        /** @brief Copies pad @p number's standard and raw state. @return `NotFound` if it isn't attached. */
        Error state(uint8_t number, PadState &standard, RawPad &raw);

        /** @brief Adds @p listener: it hears every pad's events until `unlisten()`. */
        Error listen(Listener &listener);

        /** @brief Removes @p listener; after it returns, nothing is put in it. */
        void unlisten(Listener &listener);

} // namespace Pads

BMDS_NAMESPACE_END
