/**
 * @file      guistyle.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The toolkit's fonts (devplan/fonts.md §6i): each role's font as the settings last said, opened
 * from the system's library and again when SystemFonts says a font/ setting changed, after which
 * every application that follows restyles its windows on its own loop.
 */

#include <bmds/guiapplication.h>
#include <bmds/lineeditmodel.h>
#include <bmds/log.h>
#include <bmds/mutex.h>
#include <bmds/spinlock.h>
#include <bmds/staticobject.h>
#include <bmds/systemfonts.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t Roles = size_t(FontName::Role::Terminal) + 1;

        struct Fonts {
                        Font role[Roles];
        };

        constinit SpinLock            createLock;
        constinit SpinLock            fontsLock; // the fonts, copied in and out
        constinit StaticObject<Mutex> mutex;     // the first opening, and the followers
        constinit StaticObject<Fonts> fonts;
        constinit GuiApplication     *followers[GuiStyle::MaxFollowers] = {};
        constinit std::atomic<bool>   ready{false};

        Mutex &lock() {
                SpinLock::Locker l(createLock);
                if (!mutex.isCreated()) mutex.create();
                return *mutex;
        }

        // Each role's font from the system's library (none for Role::None).
        void openAll(Font (&out)[Roles]) {
                FontLibrary &library = SystemFonts::library();
                for (size_t i = 1; i < Roles; ++i) {
                        FontName name;
                        name.role = FontName::Role(i);
                        auto [font, err] = library.open(name);
                        if (err.isOk()) out[i] = font;
                }
                return;
        }

        void restyleApplication(void *arg) {
                static_cast<GuiApplication *>(arg)->restyle();
                return;
        }

        // A font/ setting was applied (the settings' task): the fonts opened again, the followers told.
        void changed(void *) {
                Font fresh[Roles];
                openAll(fresh);
                {
                        SpinLock::Locker l(fontsLock);
                        for (size_t i = 0; i < Roles; ++i) fonts->role[i] = fresh[i];
                }
                Mutex::Locker l(lock(), WaitForever);
                for (GuiApplication *app : followers)
                        if (app != nullptr) (void)app->post(restyleApplication, app);
                return;
        }

        void ensure() {
                if (ready.load(std::memory_order_acquire)) return;
                Mutex::Locker l(lock(), WaitForever);
                if (ready.load(std::memory_order_relaxed)) return;
                Font first[Roles];
                openAll(first);
                fonts.create();
                for (size_t i = 0; i < Roles; ++i) fonts->role[i] = first[i];
                if (Error err = SystemFonts::watch(changed, nullptr); err.isError())
                        bmdsWarn("gui: font settings not followed: %s", err.desc());
                ready.store(true, std::memory_order_release);
                return;
        }

} // namespace

Font GuiStyle::font(FontName::Role role) {
        ensure();
        const size_t     i = size_t(role) < Roles && role != FontName::Role::None ? size_t(role) : 1;
        SpinLock::Locker l(fontsLock);
        return fonts->role[i];
}

size_t GuiStyle::wrap(StringView text, const Font &font, uint32_t width, Span<StringView> lines) {
        const TextWrap::Measure measure = [](void *arg, StringView piece) {
                return static_cast<const Font *>(arg)->textWidth(piece);
        };
        return TextWrap::wrap(text, width, measure, const_cast<Font *>(&font), lines);
}

Error GuiStyle::follow(GuiApplication &app) {
        ensure();
        Mutex::Locker l(lock(), WaitForever);
        for (GuiApplication *&slot : followers)
                if (slot == &app) return Error::Ok;
        for (GuiApplication *&slot : followers)
                if (slot == nullptr) {
                        slot = &app;
                        return Error::Ok;
                }
        return Error::Full;
}

void GuiStyle::unfollow(GuiApplication &app) {
        Mutex::Locker l(lock(), WaitForever);
        for (GuiApplication *&slot : followers)
                if (slot == &app) slot = nullptr;
        return;
}

BMDS_NAMESPACE_END
