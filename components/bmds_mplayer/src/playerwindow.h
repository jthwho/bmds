/**
 * @file      playerwindow.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/detailsview.h>
#include <bmds/filedialog.h>
#include <bmds/furniture.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/itemmodel.h>
#include <bmds/menu.h>
#include <bmds/slider.h>
#include <bmds/string.h>
#include <bmds/tabwidget.h>

#include "player.h"
#include "playlist.h"
#include "trackclock.h"

BMDS_NAMESPACE_BEGIN

class PlayerWindow;

/** @brief The playlist as rows: title (the playing entry or a failed one marked), artist, album, length, format. */
class PlaylistModel final : public ItemModel {
        public:
                PlaylistModel(const Playlist &playlist, const PlayerWindow &window)
                    : _playlist(playlist), _window(window) {}

                size_t      count() const override { return _playlist.count(); }
                StringView  text(size_t row) const override;
                const Icon *icon(size_t row, uint16_t size) const override;
                size_t      columns() const override { return 5; }
                StringView  columnTitle(size_t column) const override;
                StringView  columnText(size_t row, size_t column, Span<char> buffer) const override;
                Align       columnAlign(size_t column) const override;
                uint16_t    columnWidth(size_t column) const override;

        private:
                const Playlist     &_playlist;
                const PlayerWindow &_window;
};

/** @brief The playlist's view: the player's keys first, and a failed entry's error as its tooltip. */
class PlaylistView final : public DetailsView {
        public:
                PlaylistView(Widget *parent, PlayerWindow &window) : DetailsView(parent), _window(window) {}

                bool       keyPressed(const KeyEvent &event) override;
                void       mouseMoved(const WindowManager::MouseEvent &event) override;
                void       mouseLeft() override;
                StringView toolTip() const override;

        private:
                PlayerWindow &_window;
                int           _hovered = -1;
};

/**
 * @brief The media player's window (`devplan/mediaplayer.md` §6c): what is
 *        playing, the position, the transport and the volume, the playlist;
 *        it drives the playback thread (`Player`) and follows its reports.
 *
 * @par Thread Safety
 * The application's lock (the loop's thread); `reportWaiting()` from the
 * playback thread.
 */
class PlayerWindow final : public GuiWindow {
        public:
                /** @brief What a service message asks (`mplayer FILE...` with a player running). */
                enum class Request : uint32_t {
                        Show = 1, ///< The window shown.
                        Open = 2, ///< The files replace the playlist and play.
                        Add = 3,  ///< The files are added.
                };

                PlayerWindow(GuiApplication &app, const Config &config, Player &player, StringView cwd);
                ~PlayerWindow() override;

                /** @brief Makes its widgets and menus. @return `Ok` or `NoMem`. */
                Error build();

                /**
                 * @brief Adds @p paths (absolute: sound files, playlists, folders) to
                 *        the playlist; @p replace: instead of it, and plays the first.
                 */
                void addPaths(Span<const StringView> paths, bool replace);

                /** @brief The playback thread has a report (any thread). */
                static void reportWaiting(void *arg);

                /** @brief Returns the entry playing (`Playlist::NoEntry`: none) for the list's mark. */
                size_t markedEntry() const { return _mode == Mode::Stopped ? Playlist::NoEntry : _playlist.current(); }

                /** @brief Returns true while paused. */
                bool isPaused() const { return _mode == Mode::Paused; }

                /** @brief Returns row @p row's mark: playing, paused, failed, or null. */
                const Icon *markOf(size_t row) const;

                /** @brief Returns @p error said for a person (a failed entry's). */
                static const char *errorText(Error error);

                /** @brief The player's own keys. @return True if used. */
                bool playerKey(const KeyEvent &event);

                /** @brief Returns the playlist. */
                const Playlist &playlist() const { return _playlist; }

        protected:
                void closedByManager() override;
                bool keyUnused(const KeyEvent &event) override;

        private:
                enum class Mode : uint8_t {
                        Stopped,
                        Playing,
                        Paused,
                };

                // What is known of a track the thread was asked to play, for showing it.
                struct TrackText {
                                uint32_t track = 0;
                                String   path{MemSpace::Psram};
                                String   title{MemSpace::Psram}; // or the file's name
                                String   artist{MemSpace::Psram};
                                String   album{MemSpace::Psram};
                                char     format[64] = {};
                                uint64_t frames = Decoder::UnknownFrames;
                                uint32_t rateHz = 0;
                                bool     estimated = false;
                };

                static constexpr size_t MaxTrackTexts = TrackClock::MaxMarks + 1;

                Error       buildMenus();
                Error       fillPlaybackMenu();
                Error       buildControls(BoxLayout *column);
                static void chosen(void *arg, int item);
                static void transportClicked(void *arg, Button &button);
                static void seekChanged(void *arg, Slider &slider, int32_t value);
                static void seekReleased(void *arg, Slider &slider, int32_t value);
                static void volumeChanged(void *arg, Slider &slider, int32_t value);
                static void activated(void *arg, ItemView &view, size_t row);
                static void selectionChanged(void *arg, ItemView &view);
                static void filesChosen(void *arg, FileDialog &dialog, int result);
                static void reportsDue(void *arg);
                static void tick(void *arg);
                static void messageDone(void *arg);

                void       takeReports();
                void       started(const Player::Report &r);
                void       ended();
                void       failed(const Player::Report &r);
                void       playEntry(size_t index, Player::Start how);
                void       playPause();
                void       stopPlaying();
                void       next();
                void       previous();
                void       seekBy(int64_t ms);
                void       seekTo(uint64_t ms);
                void       removeSelected();
                void       moveCurrent(int by);
                void       setRepeat(Playlist::Repeat repeat);
                void       setShuffled(bool shuffled);
                void       setMuted(bool muted);
                void       applyGain();
                uint32_t   newTrack();
                TrackText *textOf(uint32_t track);
                TrackText &newText(uint32_t track);
                uint64_t   positionMs(); // of the track heard; showing it if it changed
                void       showTrack(const TrackText *text);
                void       showPosition();
                void       showMode(); // the play button, the status, the timer
                void       showList(bool rowsChanged);
                void       showCounts();
                void       showVolume();
                void       say(StringView text); // the status bar's message, for a while
                void       addPath(StringView path, size_t depth);
                void       addFolder(StringView path, size_t depth);
                void       addPlaylistFile(StringView path);
                Error      appendEntry(StringView path);

                Player           &_player;
                String            _cwd{MemSpace::Psram};
                Playlist          _playlist;
                PlaylistModel     _model{_playlist, *this};
                TrackClock        _clock;
                TrackText         _texts[MaxTrackTexts];
                size_t            _nextText = 0;
                Mode              _mode = Mode::Stopped;
                uint32_t          _lastTrack = 0;  // numbers given
                uint32_t          _requested = 0;  // the track the thread was last asked for (0: none)
                uint32_t          _shownTrack = 0; // the one in the labels
                size_t            _failedInRow = 0;
                bool              _seeking = false; // a seek asked: its target shown until it starts
                uint64_t          _seekMs = 0;
                bool              _muted = false;
                bool              _addedSome = false; // addPath() added an entry
                std::atomic<bool> _reportPosted{false};
                int               _timer = -1;
                int               _messageTimer = -1;
                const Icon       *_playIcon = nullptr; // the theme's, pinned
                const Icon       *_pauseIcon = nullptr;
                const Icon       *_errorIcon = nullptr;

                Menu          _fileMenu;
                Menu          _playbackMenu;
                Menu          _repeatMenu;
                Menu          _helpMenu;
                FileDialog    _openDialog;
                FileDialog    _addDialog;
                FileDialog    _folderDialog;
                Label        *_title = nullptr;
                Label        *_subtitle = nullptr;
                Label        *_format = nullptr;
                Label        *_elapsed = nullptr;
                Label        *_left = nullptr;
                Slider       *_seek = nullptr;
                Button       *_playButton = nullptr;
                Button       *_repeatButton = nullptr;
                Button       *_shuffleButton = nullptr;
                Button       *_muteButton = nullptr;
                Slider       *_volume = nullptr;
                TabWidget    *_tabs = nullptr;
                PlaylistView *_list = nullptr;
                StatusBar    *_status = nullptr;
};

BMDS_NAMESPACE_END
