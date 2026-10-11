/**
 * @file      trayprotocol.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <bmds/config.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The system tray's messages (`devplan/mediaplayer.md` §6e), over
 *        window manager services: a program's `TrayIcon` sends to the tray's
 *        service `tray`; the tray replies to the program's `tray.<connection>`.
 *
 * Each message is a struct starting with `uint32_t size` (its struct's size
 * as the sender knew it) and a `Kind`. Fields are appended, never changed or
 * moved: `read()` takes what the sender sent and leaves the fields it didn't
 * know zero. Strings end in a NUL within their arrays.
 *
 * @par Thread Safety
 * Values.
 */
namespace TrayProtocol {

        /** @brief The tray's service. */
        constexpr StringView Service("tray");

        /** @brief A reply service's name: `tray.` and the connection's number. */
        constexpr size_t MaxReplyName = 31;

        /** @brief A tooltip's longest text in bytes: `Update::toolTip` and `toolTipMore` together. */
        constexpr size_t MaxToolTip = 383;

        /** @brief An icon's side in pixels (`Pixels`). */
        constexpr uint32_t IconSide = 16;

        /** @brief What a message is. */
        enum class Kind : uint32_t {
                // To the tray.
                Update = 1, ///< An icon added or changed: `Update`.
                Pixels = 2, ///< An icon's own pixels: `Pixels`.
                Remove = 3, ///< An icon gone: `Header`.
                // To a program.
                Activated = 101, ///< Clicked: `Event` (reason, the icon's rectangle).
                Scrolled = 102,  ///< The wheel over it: `Event` (notches, the rectangle).
                Geometry = 103,  ///< Its place on the screen, when it changes: `Event`.
        };

        /** @brief Where the tray puts an icon: by category, then in the order they came. */
        enum class Category : uint32_t {
                Application = 0,
                Communications = 1,
                System = 2,
                Hardware = 3, ///< At the right end (the volume).
        };

        /** @brief Why `Activated` (Qt's `QSystemTrayIcon::ActivationReason`). */
        enum class Reason : uint32_t {
                Trigger = 1,     ///< A left click.
                DoubleClick = 2, ///< A second left click soon after (a `Trigger` came first).
                MiddleClick = 3,
                Context = 4, ///< A right click: the program opens its menu at the rectangle.
        };

        /** @brief `Update`'s flags. */
        enum Flag : uint32_t {
                Visible = 1u << 0,   ///< Shown (else kept, hidden).
                HasPixels = 1u << 1, ///< Its pixels follow (`Pixels`); `icon` unused.
        };

        /** @brief What every message to the tray starts with. */
        struct Header {
                        uint32_t size = sizeof(Header);
                        Kind     kind = Kind::Remove;
                        uint32_t id = 0;                       ///< The icon's number, the program's own.
                        char     reply[MaxReplyName + 1] = {}; ///< The program's reply service.
        };

        /** @brief An icon added or changed (all of it, each time). */
        struct Update {
                        uint32_t size = sizeof(Update);
                        Kind     kind = Kind::Update;
                        uint32_t id = 0;
                        char     reply[MaxReplyName + 1] = {};
                        Category category = Category::Application;
                        uint32_t flags = 0;     ///< `Flag` bits.
                        char     icon[64] = {}; ///< A theme's icon name.
                        char     toolTip[128] = {};
                        /** The tooltip past `toolTip`'s 127 bytes (appended 2026-10-11): `setToolTip()`. */
                        char toolTipMore[256] = {};
        };

        /** @brief An icon's own pixels, ARGB (straight alpha), row by row. */
        struct Pixels {
                        uint32_t size = sizeof(Pixels);
                        Kind     kind = Kind::Pixels;
                        uint32_t id = 0;
                        char     reply[MaxReplyName + 1] = {};
                        uint32_t width = IconSide;
                        uint32_t height = IconSide;
                        uint32_t argb[IconSide * IconSide] = {};
        };

        /** @brief What the tray tells a program. */
        struct Event {
                        uint32_t size = sizeof(Event);
                        Kind     kind = Kind::Activated;
                        uint32_t id = 0;
                        Reason   reason = Reason::Trigger; ///< `Activated`.
                        int32_t  notches = 0;              ///< `Scrolled`: up positive.
                        int16_t  x = 0;                    ///< The icon on the screen.
                        int16_t  y = 0;
                        uint16_t width = 0;
                        uint16_t height = 0;
        };

        /** @brief Returns @p message's kind (0 if too short to say). */
        inline uint32_t kindOf(ByteSpan message) {
                uint32_t h[2] = {};
                if (message.size() < sizeof(h)) return 0;
                std::memcpy(h, message.data(), sizeof(h));
                return h[1];
        }

        /**
         * @brief Reads @p message into @p out: the fields the sender knew, the rest
         *        zero (strings may lack their NUL: read them with `view()`). False
         *        if it is shorter than its own `size`
         *        says, or than the fields every version has (`size` and `kind`).
         */
        template <typename T> bool read(ByteSpan message, T &out) {
                uint32_t size = 0;
                if (message.size() < 2 * sizeof(uint32_t)) return false;
                std::memcpy(&size, message.data(), sizeof(size));
                if (size < 2 * sizeof(uint32_t) || size > message.size()) return false;
                std::memset(static_cast<void *>(&out), 0, sizeof(T));
                std::memcpy(static_cast<void *>(&out), message.data(), size < sizeof(T) ? size : sizeof(T));
                return true;
        }

        /** @brief Returns @p value's bytes, to send. */
        template <typename T> ByteSpan bytes(const T &value) {
                return ByteSpan(reinterpret_cast<const uint8_t *>(&value), sizeof(T));
        }

        /** @brief Copies @p text into @p out (cut on a character's boundary, NUL-ended). */
        template <size_t N> void copy(char (&out)[N], StringView text) {
                size_t n = text.size() < N - 1 ? text.size() : N - 1;
                while (n > 0 && n < text.size() && (uint8_t(text[n]) & 0xc0) == 0x80) --n; // a character's middle
                std::memcpy(out, text.data(), n);
                out[n] = '\0';
                return;
        }

        /** @brief Returns @p text (a received array) as a view, to its NUL. */
        template <size_t N> StringView view(const char (&text)[N]) {
                return StringView(text, ::strnlen(text, N));
        }

        /**
         * @brief Puts @p text (cut at `MaxToolTip` bytes on a character's boundary) in
         *        @p u's `toolTip` and, past its 127 bytes, `toolTipMore`.
         */
        inline void setToolTip(Update &u, StringView text) {
                copy(u.toolTip, text);
                const size_t first = ::strlen(u.toolTip);
                copy(u.toolTipMore, first < text.size() ? text.mid(first) : StringView());
                return;
        }

        /** @brief Copies @p u's tooltip, both parts, into @p out (NUL-ended). */
        inline void toolTipOf(const Update &u, char (&out)[MaxToolTip + 1]) {
                const StringView first = view(u.toolTip);
                const StringView more = view(u.toolTipMore);
                std::memcpy(out, first.data(), first.size());
                std::memcpy(out + first.size(), more.data(), more.size()); // 127 + 256 at most
                out[first.size() + more.size()] = '\0';
                return;
        }

} // namespace TrayProtocol

BMDS_NAMESPACE_END
