/**
 * @file      wmconfig.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The window manager's palette as system configuration parameters
 * (devplan/sysconf.md §8): `wm/palette/<role>`, by the roles' names.
 */

#include <cstdio>
#include <sdkconfig.h>
#include <bmds/staticobject.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>

#include "wmconfig.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        // A role's default, as the palette has it.
        template <Role R> constexpr uint32_t standard() {
                return WindowPalette().color(R);
        }

#define BMDS_PALETTE_PARAM(Id, Name, R, Rgb)                                                                           \
        BMDS_SYSCONF(palette##Id, "wm/palette/" Name, Config::Type::Color, Config::ColorText<Rgb>,                     \
                     "The window manager's " Name " color", 0, 0, nullptr, nullptr)

        // The desktop's default is the build's (`win start` without a color).
        BMDS_PALETTE_PARAM(Desktop, "desktop", Desktop, uint32_t(CONFIG_BMDS_DESKTOP_COLOR))
        BMDS_PALETTE_PARAM(ActiveFrame, "active-frame", ActiveFrame, standard<Role::ActiveFrame>())
        BMDS_PALETTE_PARAM(InactiveFrame, "inactive-frame", InactiveFrame, standard<Role::InactiveFrame>())
        BMDS_PALETTE_PARAM(ActiveTitle, "active-title", ActiveTitle, standard<Role::ActiveTitle>())
        BMDS_PALETTE_PARAM(InactiveTitle, "inactive-title", InactiveTitle, standard<Role::InactiveTitle>())
        BMDS_PALETTE_PARAM(CloseBox, "close-box", CloseBox, standard<Role::CloseBox>())
        BMDS_PALETTE_PARAM(CloseMark, "close-mark", CloseMark, standard<Role::CloseMark>())
        BMDS_PALETTE_PARAM(Client, "client", Client, standard<Role::Client>())
        BMDS_PALETTE_PARAM(Outline, "outline", Outline, standard<Role::Outline>())
        BMDS_PALETTE_PARAM(CursorEdge, "cursor-edge", CursorEdge, standard<Role::CursorEdge>())
        BMDS_PALETTE_PARAM(CursorFill, "cursor-fill", CursorFill, standard<Role::CursorFill>())
        BMDS_PALETTE_PARAM(Button, "button", Button, standard<Role::Button>())
        BMDS_PALETTE_PARAM(ButtonMark, "button-mark", ButtonMark, standard<Role::ButtonMark>())
        BMDS_PALETTE_PARAM(Face, "face", Face, standard<Role::Face>())
        BMDS_PALETTE_PARAM(FaceText, "face-text", FaceText, standard<Role::FaceText>())
        BMDS_PALETTE_PARAM(FaceHighlight, "face-highlight", FaceHighlight, standard<Role::FaceHighlight>())
        BMDS_PALETTE_PARAM(FaceLight, "face-light", FaceLight, standard<Role::FaceLight>())
        BMDS_PALETTE_PARAM(FaceShadow, "face-shadow", FaceShadow, standard<Role::FaceShadow>())
        BMDS_PALETTE_PARAM(FaceDark, "face-dark", FaceDark, standard<Role::FaceDark>())
        BMDS_PALETTE_PARAM(Highlight, "highlight", Highlight, standard<Role::Highlight>())
        BMDS_PALETTE_PARAM(HighlightText, "highlight-text", HighlightText, standard<Role::HighlightText>())
        BMDS_PALETTE_PARAM(DisabledText, "disabled-text", DisabledText, standard<Role::DisabledText>())
        BMDS_PALETTE_PARAM(DesktopText, "desktop-text", DesktopText, standard<Role::DesktopText>())

#undef BMDS_PALETTE_PARAM

        static_assert(WindowPalette::RoleCount == 23, "a parameter for each role");

        constinit StaticObject<SysConf::Watcher> watcher;
        constinit WindowManager                 *followed = nullptr;

        void changed(void *) {
                if (followed != nullptr) (void)followed->setPalette(WmConfig::palette());
                return;
        }

} // namespace

bool WmConfig::keyOf(StringView name, char (&key)[48]) {
        if (WindowPalette::find(name).error().isError()) return false;
        std::snprintf(key, sizeof(key), "wm/palette/%.*s", int(name.size()), name.data());
        return true;
}

WindowPalette WmConfig::palette() {
        WindowPalette palette;
        palette.setColor(Role::Desktop, CONFIG_BMDS_DESKTOP_COLOR);
        for (size_t i = 0; i < WindowPalette::RoleCount; ++i) {
                const Role role = static_cast<Role>(i);
                char       key[48];
                if (!keyOf(StringView(WindowPalette::name(role)), key)) continue;
                const Result<uint32_t> rgb = SysConf::getColor(key);
                if (rgb.isOk()) palette.setColor(role, rgb.value());
        }
        return palette;
}

Error WmConfig::follow(WindowManager &wm) {
        if (!watcher.isCreated()) watcher.create();
        watcher->stop();
        followed = &wm;
        return watcher->start("wm/palette", changed, nullptr);
}

BMDS_NAMESPACE_END
