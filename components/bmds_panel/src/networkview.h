/**
 * @file      networkview.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/netstatus.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The tray's network icon's pure parts (`devplan/network.md` §17, §20):
 *        an interface's state, the 16 × 16 picture of it, and the lines that
 *        describe it in the tooltip and the menu.
 *
 * The picture is an RJ45 socket seen from the front, its two lights showing
 * the state: both green when connected; the left one amber while there is a
 * link but no address; both dark, the picture dimmed and crossed out in red
 * with no link; dark and dimmed when off.
 *
 * @par Thread Safety
 * Reentrant.
 */
struct NetworkView {
                /** @brief The picture's side, in pixels. */
                static constexpr uint16_t Side = 16;

                /** @brief A line's longest text with its terminator (a menu item's). */
                static constexpr size_t LineBytes = 48;

                /** @brief The most lines one interface takes. */
                static constexpr size_t MaxLines = 6;

                /** @brief The colours (0xAARRGGBB). */
                static constexpr uint32_t BodyColor = 0xff505050;
                static constexpr uint32_t EdgeColor = 0xffb8b8b8;
                static constexpr uint32_t HoleColor = 0xff101010;
                static constexpr uint32_t PinColor = 0xffd8b030;
                static constexpr uint32_t DarkLight = 0xff202020;
                static constexpr uint32_t GreenLight = 0xff30e030;
                static constexpr uint32_t AmberLight = 0xffffb000;
                static constexpr uint32_t CrossColor = 0xffe02020;

                /** @brief The lights' pixels: the left one's top-left corner, and the right one's. */
                static constexpr uint16_t LightY = 12;
                static constexpr uint16_t LeftLightX = 2;
                static constexpr uint16_t RightLightX = 12;

                /** @brief An interface's state, worst first: a better one wins the icon. */
                enum class State : uint8_t {
                        Off,       ///< Disabled, or IPv4 off.
                        NoLink,    ///< The cable is out.
                        Acquiring, ///< A link, no address yet (DHCP asking, or static without one).
                        Connected, ///< A link and an address.
                };

                /** @brief Returns @p info's state. */
                static State stateOf(const NetStatus::Info &info);

                /** @brief Returns the best state of @p interfaces but the loopback (`Off` if none). */
                static State best(Span<const NetStatus::Info> interfaces);

                /** @brief Draws @p state into @p out (rows top first). */
                static void draw(State state, uint32_t (&out)[Side * Side]);

                /**
                 * @brief Writes @p info's lines into @p out: its state ("eth0: connected, 100
                 *        Mbit/s full duplex"), the address and gateway, DHCP's server and the
                 *        lease's length (or "static"), with @p leaseLeft the lease's time left,
                 *        the DNS servers, the MAC address; as many as the state has.
                 * @return The number of lines written.
                 */
                static size_t lines(const NetStatus::Info &info, bool leaseLeft, char (&out)[MaxLines][LineBytes]);

                /**
                 * @brief Writes the tooltip for @p interfaces (the loopback left out) into @p out:
                 *        each interface's lines (without the lease's time left, which would go
                 *        stale), all but each one's first indented by two spaces.
                 * @return The bytes written, without the terminator.
                 */
                static size_t toolTip(Span<const NetStatus::Info> interfaces, Span<char> out);

                /** @brief Writes @p seconds as "2 h", "1 h 52 min", "45 min" or "30 s" into @p out. */
                static void formatSpan(uint32_t seconds, char (&out)[24]);
};

BMDS_NAMESPACE_END
