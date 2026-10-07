/**
 * @file      windowpalette.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The window manager's colors by role (devplan/windows.md W12).
 */

#include <bmds/windowpalette.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // By role; the names are an interface (`win color`): append only.
        // clang-format off
        constexpr const char *Names[WindowPalette::RoleCount] = {
                "desktop",
                "active-frame",
                "inactive-frame",
                "active-title",
                "inactive-title",
                "close-box",
                "close-mark",
                "client",
                "outline",
                "cursor-edge",
                "cursor-fill",
                "button",
                "button-mark",
                "face",
                "face-text",
                "face-highlight",
                "face-light",
                "face-shadow",
                "face-dark",
                "highlight",
                "highlight-text",
                "disabled-text",
        };
        // clang-format on

} // namespace

const char *WindowPalette::name(Role role) {
        const auto i = static_cast<size_t>(role);
        return i < RoleCount ? Names[i] : nullptr;
}

Result<WindowPalette::Role> WindowPalette::find(StringView name) {
        for (size_t i = 0; i < RoleCount; ++i)
                if (name == StringView(Names[i])) return makeResult(static_cast<Role>(i));
        return makeError<Role>(Error::NotFound);
}

BMDS_NAMESPACE_END
