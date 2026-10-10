/**
 * @file      volumecontrol.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/menu.h>
#include <bmds/slider.h>
#include <bmds/trayicon.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

class VolumeControl;

/**
 * @brief The volume control's popup (`devplan/gui.md` §4s): the level over a
 *        vertical slider from 0 dBFS down to `VolumeControl::FloorDb`, and below
 *        that a notch for Mute; under it a Mute button, pressed in while muted
 *        (unmuting returns to the level). Escape or a press outside closes it.
 *
 * @par Thread Safety
 * The application's lock.
 */
class VolumePopup final : public GuiWindow {
        public:
                VolumePopup(GuiApplication &app, VolumeControl &control);

                /** @brief Opens it by @p anchor (the tray icon, on the screen). */
                void open(const Rect &anchor);

                /** @brief Shows @p db and @p muted (the settings changed). */
                void refresh(int32_t db, bool muted);

                void paint(Painter &painter) override;
                bool wheelTurned(int notches) override; // anywhere on it: the slider's

        protected:
                bool keyUnused(const KeyEvent &event) override;
                void resized() override;
                void closedByManager() override;

        private:
                static void changed(void *arg, Slider &slider, int32_t value);
                static void muteClicked(void *arg, Button &button);
                void        showLevel(int32_t value);

                VolumeControl &_item;
                Label         *_level = nullptr;
                Slider        *_slider = nullptr;
                Button        *_mute = nullptr;
};

/**
 * @brief The volume control (`devplan/audio.md` §17, `devplan/gui.md` §4s,
 *        `devplan/mediaplayer.md` §6e): a tray icon in the panel's own
 *        process, through the same messages as any program's. A speaker (the
 *        theme's, muted when muted), the level in its tooltip; a click opens
 *        the slider by it, the wheel moves the level a dB a notch, the middle
 *        button mutes, a right click opens Mute and Mixer.... Only the settings
 *        (`audio/dsp0/volume`, `audio/dsp0/mute`), as any program could; they
 *        are saved 2 s after the last change.
 *
 * @par Thread Safety
 * The application's lock.
 */
class VolumeControl {
        public:
                /** @brief The settings it shows and sets. */
                static constexpr const char *VolumeKey = "audio/dsp0/volume";
                static constexpr const char *MuteKey = "audio/dsp0/mute";
                static constexpr const char *Prefix = "audio/dsp0";

                /** @brief The quietest level before Mute (the setting's least). */
                static constexpr int32_t FloorDb = -60;

                /** @brief The levels its icon changes at: below `LowDb` no waves, from `HighDb` two. */
                static constexpr int32_t LowDb = -40;
                static constexpr int32_t HighDb = -15;

                explicit VolumeControl(GuiApplication &app);
                ~VolumeControl();
                VolumeControl(const VolumeControl &) = delete;
                VolumeControl &operator=(const VolumeControl &) = delete;

                /** @brief Shows its icon in the tray (now, or when there is one). */
                Error start();

                /** @brief Reads the settings again (they changed, here or elsewhere). */
                void refresh();

                /** @brief Sets the level (dB) and unmutes, or mutes (@p db below `FloorDb`). */
                void setLevel(int32_t db);

                /** @brief Mutes, or unmutes back to the level. */
                void setMuted(bool muted);

                /** @brief Returns true if muted. */
                bool isMuted() const { return _muted; }

                /** @brief The popup closed. */
                void popupClosed() {}

        private:
                static constexpr uint32_t SaveDelayMs = 2000; // after the last change: /boot once

                static void save(void *arg);
                static void activated(void *arg, TrayIcon &icon, TrayIcon::Reason reason);
                static void scrolled(void *arg, TrayIcon &icon, int notches);
                static void chosen(void *arg, int item);
                void        set(const char *key, StringView value);
                void        openPopup();
                void        fillMenu();

                GuiApplication &_app;
                TrayIcon        _icon;
                Menu            _menu;
                VolumePopup    *_popup = nullptr; // PSRAM, made when first opened
                int32_t         _db = -9;
                bool            _muted = false;
                int             _saveTimer = -1;
                char            _tip[32] = {};
};

BMDS_NAMESPACE_END
