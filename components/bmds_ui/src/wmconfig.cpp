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
        BMDS_PALETTE_PARAM(ToolTip, "tooltip", ToolTip, standard<Role::ToolTip>())
        BMDS_PALETTE_PARAM(ToolTipText, "tooltip-text", ToolTipText, standard<Role::ToolTipText>())

#undef BMDS_PALETTE_PARAM

        static_assert(WindowPalette::RoleCount == 25, "a parameter for each role");

        constinit StaticObject<SysConf::Watcher> watcher;
        constinit WindowManager                 *followed = nullptr;

        // Each role's parameter, found once (devplan/sysconf.md §12.1); before the watcher starts,
        // so a notification only reads them.
        constinit SysConf::Id roleIds[WindowPalette::RoleCount] = {};
        constinit bool        idsFound = false;

        void findIds() {
                if (idsFound || !SysConf::isReady()) return;
                for (size_t i = 0; i < WindowPalette::RoleCount; ++i) {
                        char key[48];
                        std::snprintf(key, sizeof(key), "wm/palette/%s", WindowPalette::name(static_cast<Role>(i)));
                        const Result<SysConf::Id> id = SysConf::find(key);
                        roleIds[i] = id.isOk() ? id.value() : SysConf::NoId;
                }
                idsFound = true;
                return;
        }

        // A palette color changed (notifications are one at a time): the roles it names, by id,
        // into the window manager's palette; everything after an overflow.
        void changed(void *) {
                if (followed == nullptr) return;
                WindowPalette palette = followed->palette();
                SysConf::Id   id;
                while (watcher->next(id)) {
                        for (size_t i = 0; i < WindowPalette::RoleCount; ++i) {
                                if (roleIds[i] != id) continue;
                                const Result<uint32_t> rgb = SysConf::getColor(id);
                                if (rgb.isOk()) palette.setColor(static_cast<Role>(i), rgb.value());
                        }
                }
                if (watcher->overflowed()) palette = WmConfig::palette();
                (void)followed->setPalette(palette);
                return;
        }

} // namespace

WindowPalette WmConfig::palette() {
        findIds();
        WindowPalette palette;
        palette.setColor(Role::Desktop, CONFIG_BMDS_DESKTOP_COLOR);
        for (size_t i = 0; i < WindowPalette::RoleCount; ++i) {
                const Result<uint32_t> rgb = SysConf::getColor(roleIds[i]);
                if (rgb.isOk()) palette.setColor(static_cast<Role>(i), rgb.value());
        }
        return palette;
}

Error WmConfig::follow(WindowManager &wm) {
        findIds();
        if (!watcher.isCreated()) watcher.create();
        watcher->stop();
        followed = &wm;
        return watcher->start("wm/palette", changed, nullptr);
}

BMDS_NAMESPACE_END
