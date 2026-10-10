/**
 * @file      playerwindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The media player's window (devplan/mediaplayer.md §6c): what is playing, the position, the
 * transport and the volume, the playlist; it drives the playback thread and follows its reports.
 */

#include "playerwindow.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <bmds/clock.h>
#include <bmds/list.h>
#include <bmds/menubar.h>
#include <bmds/messagebox.h>
#include <bmds/sampleformat.h>
#include <bmds/vfs.h>
#include <bmds/windowstack.h>

#include "m3u.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t    TickMs = 250;     // the time shown: 4 a second, only while playing
        constexpr uint32_t    MessageMs = 5000; // a message in the status bar, then the state again
        constexpr uint64_t    RestartMs = 3000; // Previous past this: the track from its start
        constexpr int64_t     StepMs = 5000;    // Left and Right
        constexpr int32_t     QuietestDb = -60; // the volume slider's range
        constexpr int32_t     LowDb = -40;      // its icon: low below this, medium, high above HighDb
        constexpr int32_t     HighDb = -15;
        constexpr size_t      MaxDepth = 6; // Add Folder's folders within folders
        constexpr size_t      MaxPlaylistBytes = 1024 * 1024;
        constexpr size_t      MaxSettingsBytes = 4096;
        constexpr uint32_t    SaveMs = 2000; // what is kept: written this long after the last change
        constexpr const char *KeptDir = "/home/.config/mplayer";
        constexpr const char *SettingsName = "mplayer.conf";
        constexpr const char *PlaylistName = "playlist.m3u8";
        constexpr const char *AppTitle = "Media Player";

        enum Choice : int {
                ChooseOpen = 1,
                ChooseAddFiles,
                ChooseAddFolder,
                ChooseOpenPlaylist,
                ChooseSavePlaylist,
                ChooseClose,
                ChooseQuit,
                ChoosePlayPause,
                ChooseStop,
                ChoosePrevious,
                ChooseNext,
                ChooseBack,
                ChooseForward,
                ChooseRepeatOff,
                ChooseRepeatAll,
                ChooseRepeatOne,
                ChooseShuffle,
                ChooseAbout,
                ChooseShow,
        };

        // The transport's buttons, by their data.
        enum Transport : uintptr_t {
                TransportPrevious = 1,
                TransportPlay,
                TransportStop,
                TransportNext,
                TransportRepeat,
                TransportShuffle,
                TransportMute,
        };

        constexpr uint8_t KeyO = HidKey::A + ('o' - 'a'), KeyQ = HidKey::A + ('q' - 'a');
        constexpr uint8_t KeyW = HidKey::A + ('w' - 'a');

        constexpr const char *MediaPatterns = "*.mp3;*.ogg;*.oga;*.flac;*.wav;*.mid;*.midi;*.m3u;*.m3u8";

        // A button with only an icon, and its tooltip (kept: a literal).
        class IconButton final : public Button {
                public:
                        IconButton(Widget *parent, const char *tip) : Button(parent), _tip(tip) {}
                        StringView toolTip() const override { return StringView(_tip); }

                private:
                        const char *_tip;
        };

        // The volume: its tooltip the level in dB.
        class VolumeSlider final : public Slider {
                public:
                        explicit VolumeSlider(Widget *parent) : Slider(parent, Slider::Orientation::Horizontal) {}
                        StringView toolTip() const override { return StringView(_tip); }

                        void setTip(int32_t db, bool muted) {
                                if (muted) {
                                        std::snprintf(_tip, sizeof(_tip), "Volume: muted (%ld dB)", long(db));
                                } else {
                                        std::snprintf(_tip, sizeof(_tip), "Volume: %ld dB", long(db));
                                }
                                return;
                        }

                private:
                        char _tip[32] = "Volume: 0 dB";
        };

        bool endsWithNoCase(StringView s, StringView tail) {
                if (s.size() < tail.size()) return false;
                for (size_t i = 0; i < tail.size(); ++i) {
                        char c = s[s.size() - tail.size() + i];
                        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
                        if (c != tail[i]) return false;
                }
                return true;
        }

        bool isPlaylistName(StringView name) {
                return endsWithNoCase(name, ".m3u") || endsWithNoCase(name, ".m3u8");
        }

        bool isSoundName(StringView name) {
                for (const char *ext : {".mp3", ".ogg", ".oga", ".flac", ".wav", ".mid", ".midi"})
                        if (endsWithNoCase(name, StringView(ext))) return true;
                return false;
        }

        StringView fileName(StringView path) {
                const size_t slash = path.lastIndexOf('/');
                return slash == StringView::NPos ? path : path.mid(slash + 1);
        }

        int compareNoCase(StringView a, StringView b) {
                const size_t n = a.size() < b.size() ? a.size() : b.size();
                for (size_t i = 0; i < n; ++i) {
                        char x = a[i], y = b[i];
                        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
                        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
                        if (x != y) return x < y ? -1 : 1;
                }
                return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
        }

        // A track's format line: "MP3 · 44.1 kHz · stereo · 192 kbps".
        void formatLine(const Decoder::Info &info, char *out, size_t size) {
                char codec[24];
                if (info.codec == Codec::Midi) {
                        std::snprintf(codec, sizeof(codec), "MIDI");
                } else if (info.sourceBits != 0) {
                        std::snprintf(codec, sizeof(codec), "%s %u-bit", info.codec == Codec::Wav ? "WAV" : "FLAC",
                                      unsigned(info.sourceBits));
                } else {
                        std::snprintf(codec, sizeof(codec), "%s",
                                      info.codec == Codec::Mp3      ? "MP3"
                                      : info.codec == Codec::Vorbis ? "Ogg Vorbis"
                                      : info.codec == Codec::Flac   ? "FLAC"
                                                                    : "WAV");
                }
                const unsigned khz10 = unsigned((info.rateHz + 50) / 100);
                char           rate[16];
                if (khz10 % 10 == 0) {
                        std::snprintf(rate, sizeof(rate), "%u kHz", khz10 / 10);
                } else {
                        std::snprintf(rate, sizeof(rate), "%u.%u kHz", khz10 / 10, khz10 % 10);
                }
                const int n = std::snprintf(out, size, "%s \xc2\xb7 %s \xc2\xb7 %s", codec, rate,
                                            info.channels == 1 ? "mono" : "stereo");
                if (info.bitrate != 0 && info.sourceBits == 0 && info.codec != Codec::Midi && n > 0 && size_t(n) < size)
                        std::snprintf(out + n, size - size_t(n), " \xc2\xb7 %lu kbps",
                                      static_cast<unsigned long>((info.bitrate + 500) / 1000));
                return;
        }

        // A short name for a format, for the list.
        const char *formatName(Codec codec) {
                switch (codec) {
                        case Codec::Wav: return "WAV";
                        case Codec::Mp3: return "MP3";
                        case Codec::Vorbis: return "Ogg";
                        case Codec::Flac: return "FLAC";
                        case Codec::Midi: return "MIDI";
                }
                return "";
        }

} // namespace

// ---- The playlist's rows

StringView PlaylistModel::text(size_t row) const {
        const Playlist::Info info = _playlist.info(row);
        return !info.title.isEmpty() ? info.title : fileName(_playlist.path(row));
}

const Icon *PlaylistModel::icon(size_t row, uint16_t size) const {
        return size == 16 ? _window.markOf(row) : nullptr;
}

StringView PlaylistModel::columnTitle(size_t column) const {
        static constexpr const char *Titles[] = {"Title", "Artist", "Album", "Length", "Format"};
        return column < 5 ? StringView(Titles[column]) : StringView();
}

StringView PlaylistModel::columnText(size_t row, size_t column, Span<char> buffer) const {
        const Playlist::Info info = _playlist.info(row);
        switch (column) {
                case 0: return text(row);
                case 1: return info.artist;
                case 2: return info.album;
                case 3: {
                        if (info.lengthMs == 0) return StringView();
                        char time[TimeText::MaxText];
                        TimeText::format(info.lengthMs, Span<char>(time));
                        const int n =
                                std::snprintf(buffer.data(), buffer.size(), "%s%s", info.estimated ? "~" : "", time);
                        return n > 0 ? StringView(buffer.data(),
                                                  size_t(n) < buffer.size() ? size_t(n) : buffer.size() - 1)
                                     : StringView();
                }
                case 4: return info.format;
                default: return StringView();
        }
}

ItemModel::Align PlaylistModel::columnAlign(size_t column) const {
        return column == 3 ? Align::Right : Align::Left;
}

uint16_t PlaylistModel::columnWidth(size_t column) const {
        static constexpr uint16_t Widths[] = {240, 140, 140, 64, 56};
        return column < 5 ? Widths[column] : 100;
}

// ---- The playlist's view

bool PlaylistView::keyPressed(const KeyEvent &event) {
        if (_window.playerKey(event)) return true;
        return DetailsView::keyPressed(event);
}

void PlaylistView::mouseMoved(const WindowManager::MouseEvent &event) {
        DetailsView::mouseMoved(event);
        const Rect v = viewport();
        int        row = -1;
        if (event.x >= v.x && event.y >= v.y && uint32_t(event.x) < v.right() && uint32_t(event.y) < v.bottom())
                row = rowAt(contentX(event.x), contentY(event.y));
        if (row >= 0 && _window.playlist().error(size_t(row)).isOk()) row = -1;
        if (row != _hovered) {
                _hovered = row;
                _window.application().toolTipChanged(this);
        }
        return;
}

void PlaylistView::mouseLeft() {
        DetailsView::mouseLeft();
        _hovered = -1;
        return;
}

StringView PlaylistView::toolTip() const {
        if (_hovered < 0 || size_t(_hovered) >= _window.playlist().count()) return StringView();
        return StringView(PlayerWindow::errorText(_window.playlist().error(size_t(_hovered))));
}

// ---- The window

PlayerWindow::PlayerWindow(GuiApplication &app, const Config &config, Player &player, StringView cwd)
    : GuiWindow(app, config), _player(player), _fileMenu(app), _playbackMenu(app), _repeatMenu(app), _helpMenu(app),
      _trayMenu(app), _tray(app), _notifier(app), _openDialog(app, FileDialog::Mode::OpenMany, "Open"),
      _addDialog(app, FileDialog::Mode::OpenMany, "Add Files"),
      _folderDialog(app, FileDialog::Mode::Folder, "Add Folder"),
      _listDialog(app, FileDialog::Mode::Open, "Open Playlist"),
      _saveDialog(app, FileDialog::Mode::Save, "Save Playlist As") {
        (void)_cwd.assign(cwd);
        _notifier.setTold(nowPlayingTold, this);
}

PlayerWindow::~PlayerWindow() {
        if (_timer >= 0) application().stopTimer(_timer);
        if (_messageTimer >= 0) application().stopTimer(_messageTimer);
        if (_saveTimer >= 0) application().stopTimer(_saveTimer);
        application().cancelPosts(this);
}

Error PlayerWindow::build() {
        auto *column = Widget::create<BoxLayout>(this, BoxLayout::Direction::Vertical);
        if (column == nullptr) return Error::NoMem;
        setCentral(column);
        auto *bar = Widget::create<MenuBar>(column);
        if (bar == nullptr) return Error::NoMem;
        Error err = buildMenus();
        if (err.isOk()) err = bar->addMenu("&File", _fileMenu);
        if (err.isOk()) err = bar->addMenu("&Playback", _playbackMenu);
        if (err.isOk()) err = bar->addMenu("&Help", _helpMenu);
        if (err.isError()) return err;
        setMenuBar(bar);
        err = buildControls(column);
        if (err.isError()) return err;
        _tabs = Widget::create<TabWidget>(column);
        _status = Widget::create<StatusBar>(column);
        if (_tabs == nullptr || _status == nullptr) return Error::NoMem;
        _tabs->setStretch(1);
        _list = Widget::create<PlaylistView>(_tabs, *this);
        if (_list == nullptr) return Error::NoMem;
        (void)_tabs->addTab(_list, "Playlist");
        _list->setModel(&_model);
        _list->setActivated(activated, this);
        _list->setChanged(selectionChanged, this);
        (void)_status->addField(0);
        (void)_status->addField(200);

        for (FileDialog *d : {&_openDialog, &_addDialog}) {
                (void)d->addFilter("Media files", MediaPatterns);
                (void)d->addFilter("Playlists", "*.m3u;*.m3u8");
                (void)d->addFilter("All files", "");
                (void)d->setFolder("/home");
                d->setFinished(filesChosen, this);
        }
        (void)_folderDialog.setFolder("/home");
        _folderDialog.setFinished(filesChosen, this);
        (void)_listDialog.addFilter("Playlists", "*.m3u;*.m3u8");
        (void)_listDialog.addFilter("All files", "");
        (void)_listDialog.setFolder("/home");
        _listDialog.setFinished(filesChosen, this);
        (void)_saveDialog.addFilter("Playlists (UTF-8)", "*.m3u8");
        (void)_saveDialog.addFilter("Playlists", "*.m3u");
        (void)_saveDialog.setFolder("/home");
        (void)_saveDialog.setFileName("Playlist.m3u8");
        _saveDialog.setFinished(saveChosen, this);

        IconTheme &theme = application().icons();
        _playIcon = theme.find(IconId::MediaPlaybackStart, 16);
        _pauseIcon = theme.find(IconId::MediaPlaybackPause, 16);
        _errorIcon = theme.find(IconId::DialogError, 16);
        setIcon(theme.find(IconId::MultimediaPlayer, 16));
        setLargeIcon(theme.find(IconId::MultimediaPlayer, 32));

        // In the tray while it runs (devplan/mediaplayer.md §6i).
        _tray.setIcon("multimedia-player");
        _tray.setContextMenu(&_trayMenu);
        _tray.setActivated(trayActivated, this);
        showTrayTip(0);
        (void)_tray.show();

        showTrack(nullptr);
        showMode();
        showVolume();
        showCounts();
        column->relayout();
        _list->setFocus();
        return Error::Ok;
}

Error PlayerWindow::buildMenus() {
        IconTheme &theme = application().icons();
        Error      err = _fileMenu.addItem("&Open...", ChooseOpen, 0, "Ctrl+O", theme.find(IconId::DocumentOpen, 16));
        if (err.isOk())
                err = _fileMenu.addItem("&Add Files...", ChooseAddFiles, 0, StringView(),
                                        theme.find(IconId::ListAdd, 16));
        if (err.isOk())
                err = _fileMenu.addItem("Add &Folder...", ChooseAddFolder, 0, StringView(),
                                        theme.find(IconId::FolderMusic, 16));
        if (err.isOk()) err = _fileMenu.addSeparator();
        if (err.isOk()) err = _fileMenu.addItem("Open &Playlist...", ChooseOpenPlaylist);
        if (err.isOk())
                err = _fileMenu.addItem("&Save Playlist As...", ChooseSavePlaylist, 0, StringView(),
                                        theme.find(IconId::DocumentSaveAs, 16));
        if (err.isOk()) err = _fileMenu.addSeparator();
        if (err.isOk())
                err = _fileMenu.addItem("&Close", ChooseClose, 0, "Ctrl+W", theme.find(IconId::WindowClose, 16));
        if (err.isOk()) err = _fileMenu.addItem("&Quit", ChooseQuit, 0, "Ctrl+Q", theme.find(IconId::SystemLogOut, 16));
        if (err.isOk()) err = _repeatMenu.addItem("&Off", ChooseRepeatOff, MenuLayout::Checked);
        if (err.isOk()) err = _repeatMenu.addItem("&All", ChooseRepeatAll);
        if (err.isOk()) err = _repeatMenu.addItem("&One", ChooseRepeatOne);
        if (err.isOk()) err = fillPlaybackMenu();
        if (err.isOk())
                err = _helpMenu.addItem("&About", ChooseAbout, 0, StringView(), theme.find(IconId::HelpAbout, 16));
        // The tray icon's.
        if (err.isOk())
                err = _trayMenu.addItem("&Play/Pause", ChoosePlayPause, 0, StringView(),
                                        theme.find(IconId::MediaPlaybackStart, 16));
        if (err.isOk())
                err = _trayMenu.addItem("&Stop", ChooseStop, 0, StringView(),
                                        theme.find(IconId::MediaPlaybackStop, 16));
        if (err.isOk())
                err = _trayMenu.addItem("Pre&vious", ChoosePrevious, 0, StringView(),
                                        theme.find(IconId::MediaSkipBackward, 16));
        if (err.isOk())
                err = _trayMenu.addItem("&Next", ChooseNext, 0, StringView(), theme.find(IconId::MediaSkipForward, 16));
        if (err.isOk()) err = _trayMenu.addSeparator();
        if (err.isOk())
                err = _trayMenu.addItem("Show P&layer", ChooseShow, 0, StringView(),
                                        theme.find(IconId::MultimediaPlayer, 16));
        if (err.isOk())
                err = _trayMenu.addItem("&Quit", ChooseQuit, 0, StringView(), theme.find(IconId::SystemLogOut, 16));
        for (Menu *m : {&_fileMenu, &_playbackMenu, &_repeatMenu, &_helpMenu, &_trayMenu})
                m->setTriggered(chosen, this);
        return err;
}

// Playback: made again when Shuffle's mark changes.
Error PlayerWindow::fillPlaybackMenu() {
        IconTheme &theme = application().icons();
        Error      err = _playbackMenu.addItem("&Play/Pause", ChoosePlayPause, 0, "Space",
                                               theme.find(IconId::MediaPlaybackStart, 16));
        if (err.isOk())
                err = _playbackMenu.addItem("&Stop", ChooseStop, 0, StringView(),
                                            theme.find(IconId::MediaPlaybackStop, 16));
        if (err.isOk())
                err = _playbackMenu.addItem("Pre&vious", ChoosePrevious, 0, "Ctrl+Left",
                                            theme.find(IconId::MediaSkipBackward, 16));
        if (err.isOk())
                err = _playbackMenu.addItem("&Next", ChooseNext, 0, "Ctrl+Right",
                                            theme.find(IconId::MediaSkipForward, 16));
        if (err.isOk()) err = _playbackMenu.addSeparator();
        if (err.isOk())
                err = _playbackMenu.addItem("&Back 5 s", ChooseBack, 0, "Left",
                                            theme.find(IconId::MediaSeekBackward, 16));
        if (err.isOk())
                err = _playbackMenu.addItem("&Forward 5 s", ChooseForward, 0, "Right",
                                            theme.find(IconId::MediaSeekForward, 16));
        if (err.isOk()) err = _playbackMenu.addSeparator();
        if (err.isOk())
                err = _playbackMenu.addSubmenu("&Repeat", _repeatMenu, theme.find(IconId::MediaPlaylistRepeat, 16));
        if (err.isOk())
                err = _playbackMenu.addItem("S&huffle", ChooseShuffle,
                                            _playlist.isShuffled() ? MenuLayout::Checked : 0);
        return err;
}

Error PlayerWindow::buildControls(BoxLayout *column) {
        // What is playing.
        auto *info = Widget::create<BoxLayout>(column, BoxLayout::Direction::Vertical);
        if (info == nullptr) return Error::NoMem;
        info->setMargins(8, 6, 8, 2);
        info->setSpacing(2);
        _title = Widget::create<Label>(info);
        _subtitle = Widget::create<Label>(info);
        _format = Widget::create<Label>(info);
        // The position.
        auto *position = Widget::create<BoxLayout>(column, BoxLayout::Direction::Horizontal);
        if (_title == nullptr || _subtitle == nullptr || _format == nullptr || position == nullptr) return Error::NoMem;
        position->setMargins(8, 2, 8, 2);
        position->setSpacing(8);
        _elapsed = Widget::create<Label>(position, "0:00");
        _seek = Widget::create<Slider>(position, Slider::Orientation::Horizontal);
        _left = Widget::create<Label>(position);
        // The transport and the volume.
        auto *transport = Widget::create<BoxLayout>(column, BoxLayout::Direction::Horizontal);
        if (_elapsed == nullptr || _seek == nullptr || _left == nullptr || transport == nullptr) return Error::NoMem;
        _elapsed->setMinimumWidth(72);
        _elapsed->setAlign(Label::Align::Right);
        _left->setMinimumWidth(72);
        _seek->setStretch(1);
        _seek->setRange(0, 0);
        _seek->setChanged(seekChanged, this);
        _seek->setReleased(seekReleased, this);
        transport->setMargins(8, 2, 8, 6);
        transport->setSpacing(4);
        IconTheme &theme = application().icons();
        const struct {
                        Transport   which;
                        IconId      icon;
                        const char *tip;
        } buttons[] = {
                {TransportPrevious, IconId::MediaSkipBackward, "Previous (Ctrl+Left)"},
                {TransportPlay, IconId::MediaPlaybackStart, "Play or pause (Space)"},
                {TransportStop, IconId::MediaPlaybackStop, "Stop"},
                {TransportNext, IconId::MediaSkipForward, "Next (Ctrl+Right)"},
                {TransportRepeat, IconId::MediaPlaylistRepeat, "Repeat: off"},
                {TransportShuffle, IconId::MediaPlaylistShuffle, "Shuffle"},
        };
        for (const auto &b : buttons) {
                if (b.which == TransportRepeat) {
                        auto *gap = Widget::create<Label>(transport); // a label: it paints its face
                        if (gap == nullptr) return Error::NoMem;
                        gap->setMinimumWidth(8);
                }
                auto *button = Widget::create<IconButton>(transport, b.tip);
                if (button == nullptr) return Error::NoMem;
                button->setIcon(theme.find(b.icon, 16));
                button->setData(b.which);
                button->setClicked(transportClicked, this);
                if (b.which == TransportPlay) _playButton = button;
                if (b.which == TransportRepeat) _repeatButton = button;
                if (b.which == TransportShuffle) _shuffleButton = button;
        }
        auto *space = Widget::create<Label>(transport);
        if (space == nullptr) return Error::NoMem;
        space->setStretch(1);
        auto *mute = Widget::create<IconButton>(transport, "Mute");
        auto *volume = Widget::create<VolumeSlider>(transport);
        if (mute == nullptr || volume == nullptr) return Error::NoMem;
        mute->setData(TransportMute);
        mute->setClicked(transportClicked, this);
        _muteButton = mute;
        _volume = volume;
        _volume->setRange(QuietestDb, 0);
        _volume->setSteps(1, 6);
        _volume->setValue(0);
        _volume->setMaximumWidth(140);
        _volume->setStretch(0);
        _volume->setChanged(volumeChanged, this);
        return Error::Ok;
}

// ---- Menus, buttons and keys

void PlayerWindow::chosen(void *arg, int item) {
        auto *self = static_cast<PlayerWindow *>(arg);
        switch (item) {
                case ChooseOpen: (void)self->_openDialog.open(); break;
                case ChooseAddFiles: (void)self->_addDialog.open(); break;
                case ChooseAddFolder: (void)self->_folderDialog.open(); break;
                case ChooseOpenPlaylist: (void)self->_listDialog.open(); break;
                case ChooseSavePlaylist: (void)self->_saveDialog.open(); break;
                case ChooseClose: self->closeOrHide(); break;
                case ChooseQuit: self->application().quit(0); break;
                case ChooseShow: self->showPlayer(); break;
                case ChoosePlayPause: self->playPause(); break;
                case ChooseStop: self->stopPlaying(); break;
                case ChoosePrevious: self->previous(); break;
                case ChooseNext: self->next(); break;
                case ChooseBack: self->seekBy(-StepMs); break;
                case ChooseForward: self->seekBy(StepMs); break;
                case ChooseRepeatOff: self->setRepeat(Playlist::Repeat::Off); break;
                case ChooseRepeatAll: self->setRepeat(Playlist::Repeat::All); break;
                case ChooseRepeatOne: self->setRepeat(Playlist::Repeat::One); break;
                case ChooseShuffle: self->setShuffled(!self->_playlist.isShuffled()); break;
                case ChooseAbout:
                        (void)MessageBox::show(self->application(), MessageBox::Kind::Information, "About Media Player",
                                               "Media Player\n\nPlays sound files (WAV, MP3, Ogg Vorbis, FLAC) and "
                                               "MIDI files, through the synth and its SoundFont.",
                                               MessageBox::Buttons::Ok, nullptr, nullptr);
                        break;
                default: break;
        }
        return;
}

void PlayerWindow::transportClicked(void *arg, Button &button) {
        auto *self = static_cast<PlayerWindow *>(arg);
        switch (button.data()) {
                case TransportPrevious: self->previous(); break;
                case TransportPlay: self->playPause(); break;
                case TransportStop: self->stopPlaying(); break;
                case TransportNext: self->next(); break;
                case TransportRepeat: {
                        const Playlist::Repeat r = self->_playlist.repeat();
                        self->setRepeat(r == Playlist::Repeat::Off   ? Playlist::Repeat::All
                                        : r == Playlist::Repeat::All ? Playlist::Repeat::One
                                                                     : Playlist::Repeat::Off);
                        break;
                }
                case TransportShuffle: self->setShuffled(!self->_playlist.isShuffled()); break;
                case TransportMute: self->setMuted(!self->_muted); break;
                default: break;
        }
        return;
}

bool PlayerWindow::playerKey(const KeyEvent &event) {
        const bool plain = !event.hasCtrl() && !event.hasAlt() && !event.hasShift();
        const bool ctrl = event.hasCtrl() && !event.hasAlt() && !event.hasShift();
        if (plain && event.usage == HidKey::Space) {
                playPause();
        } else if (plain && event.usage == HidKey::Left) {
                seekBy(-StepMs);
        } else if (plain && event.usage == HidKey::Right) {
                seekBy(StepMs);
        } else if (ctrl && event.usage == HidKey::Left) {
                previous();
        } else if (ctrl && event.usage == HidKey::Right) {
                next();
        } else if (ctrl && event.usage == HidKey::Up) {
                moveCurrent(-1);
        } else if (ctrl && event.usage == HidKey::Down) {
                moveCurrent(1);
        } else if (plain && event.usage == HidKey::Delete) {
                removeSelected();
        } else {
                return false;
        }
        return true;
}

bool PlayerWindow::keyUnused(const KeyEvent &event) {
        const bool ctrl = event.hasCtrl() && !event.hasAlt() && !event.hasShift();
        if (ctrl && event.usage == KeyO) {
                (void)_openDialog.open();
        } else if (ctrl && event.usage == KeyQ) {
                application().quit(0);
        } else if (ctrl && event.usage == KeyW) {
                closeOrHide();
        } else {
                return playerKey(event);
        }
        return true;
}

void PlayerWindow::resized() {
        GuiWindow::resized();
        if (geometry().width != 0 && geometry().height != 0) {
                _keptWidth = geometry().width;
                _keptHeight = geometry().height;
        }
        return;
}

// Closed by its frame (or the window manager): as Close.
void PlayerWindow::closedByManager() {
        GuiWindow::closedByManager();
        if (_mode == Mode::Stopped || !_tray.isSystemTrayAvailable()) application().quit(0);
        return;
}

// While something plays, and a tray can show it again, the window goes and the player goes on;
// otherwise it quits.
void PlayerWindow::closeOrHide() {
        if (_mode == Mode::Stopped || !_tray.isSystemTrayAvailable()) {
                application().quit(0);
                return;
        }
        close();
        return;
}

void PlayerWindow::showPlayer() {
        if (_nowPlaying != 0) { // the player says it now
                (void)_notifier.close(_nowPlaying);
                _nowPlaying = 0;
        }
        if (!isShown()) {
                const Rect was = lastRect();
                if (was.width != 0 ? show(was).isError() : show().isError()) return;
        }
        activate();
        return;
}

void PlayerWindow::trayActivated(void *arg, TrayIcon &, TrayIcon::Reason reason) {
        auto *self = static_cast<PlayerWindow *>(arg);
        if (reason == TrayIcon::Reason::Trigger) self->showPlayer();
        return; // Context: its menu, opened by the icon
}

// "Title — Artist · 1:23 / 3:45", or what it is doing (sent only when it changes).
void PlayerWindow::showTrayTip(uint64_t ms) {
        const TrackText *t = _mode != Mode::Stopped ? textOf(_shownTrack) : nullptr;
        if (t == nullptr) {
                _tray.setToolTip(_mode == Mode::Stopped ? "Media Player: stopped" : "Media Player");
                return;
        }
        char      tip[128];
        const int n = std::snprintf(tip, sizeof(tip), "%.60s%s%.40s", t->title.cStr(),
                                    t->artist.isEmpty() ? "" : " \xe2\x80\x94 ", t->artist.cStr());
        if (n > 0 && size_t(n) < sizeof(tip)) {
                char        at[TimeText::MaxText];
                char        length[TimeText::MaxText] = "";
                const bool  known = t->rateHz != 0 && t->frames != Decoder::UnknownFrames;
                const char *paused = _mode == Mode::Paused ? " (paused)" : "";
                TimeText::format(ms, Span<char>(at));
                if (known) TimeText::format(t->frames * 1000 / t->rateHz, Span<char>(length));
                std::snprintf(tip + n, sizeof(tip) - size_t(n), " \xc2\xb7 %s%s%s%s", at, known ? " / " : "", length,
                              paused);
        }
        _tray.setToolTip(StringView(tip));
        return;
}

// ---- The list

void PlayerWindow::activated(void *arg, ItemView &view, size_t row) {
        auto *self = static_cast<PlayerWindow *>(arg);
        (void)view;
        if (row >= self->_playlist.count()) return;
        self->_playlist.setCurrent(row);
        self->_failedInRow = 0;
        self->playEntry(row, Player::Start::Now);
        return;
}

void PlayerWindow::selectionChanged(void *arg, ItemView &view) {
        auto *self = static_cast<PlayerWindow *>(arg);
        // A failed entry chosen: why.
        if (view.current() >= 0 && size_t(view.current()) < self->_playlist.count()) {
                const Error e = self->_playlist.error(size_t(view.current()));
                if (e.isError()) self->say(StringView(errorText(e)));
        }
        return;
}

const Icon *PlayerWindow::markOf(size_t row) const {
        if (row == markedEntry()) return _mode == Mode::Paused ? _pauseIcon : _playIcon;
        return _playlist.error(row).isError() ? _errorIcon : nullptr;
}

void PlayerWindow::showList(bool rowsChanged) {
        if (!rowsChanged) {
                _list->update();
        } else {
                // The selection kept where it can be: the current row (rows moved or went).
                const int    current = _list->current();
                const size_t keep = current >= 0 && size_t(current) < _playlist.count() ? size_t(current) : 0;
                _list->resetTo(Span<const size_t>(&keep, current >= 0 && size_t(current) < _playlist.count() ? 1 : 0),
                               current >= 0 && size_t(current) < _playlist.count() ? current : -1);
        }
        showCounts();
        return;
}

void PlayerWindow::showCounts() {
        uint64_t ms = 0;
        bool     some = false;
        for (size_t i = 0; i < _playlist.count(); ++i) {
                const uint32_t l = _playlist.info(i).lengthMs;
                ms += l;
                some = some || l == 0;
        }
        char         time[TimeText::MaxText];
        char         text[64];
        const size_t current = _playlist.current();
        if (_playlist.count() == 0) {
                std::snprintf(text, sizeof(text), "No files");
        } else if (current == Playlist::NoEntry) {
                std::snprintf(text, sizeof(text), "%zu files \xc2\xb7 %s%s", _playlist.count(),
                              TimeText::format(ms, Span<char>(time)), some ? "+" : "");
        } else {
                std::snprintf(text, sizeof(text), "%zu of %zu \xc2\xb7 %s%s", current + 1, _playlist.count(),
                              TimeText::format(ms, Span<char>(time)), some ? "+" : "");
        }
        _status->setText(1, StringView(text));
        return;
}

void PlayerWindow::removeSelected() {
        const ItemSelection &sel = _list->selection();
        if (sel.selectedCount() == 0) return;
        // From the last, so the rows before keep their numbers.
        for (size_t row = _playlist.count(); row-- > 0;)
                if (sel.isSelected(row)) _playlist.remove(row);
        _resumeEntry = Playlist::NoEntry;
        changed(true);
        const int    current = _list->current();
        const size_t keep = current >= 0 && size_t(current) < _playlist.count() ? size_t(current)
                            : _playlist.count() > 0                             ? _playlist.count() - 1
                                                                                : 0;
        _list->resetTo(Span<const size_t>(&keep, _playlist.count() > 0 ? 1 : 0),
                       _playlist.count() > 0 ? int(keep) : -1);
        showCounts();
        return;
}

void PlayerWindow::moveCurrent(int by) {
        const int row = _list->current();
        if (row < 0) return;
        const long to = long(row) + by;
        if (to < 0 || size_t(to) >= _playlist.count()) return;
        _playlist.move(size_t(row), size_t(to));
        _resumeEntry = Playlist::NoEntry;
        changed(true);
        const size_t keep = size_t(to);
        _list->resetTo(Span<const size_t>(&keep, 1), int(to));
        _list->showRow(keep);
        showCounts();
        return;
}

void PlayerWindow::setRepeat(Playlist::Repeat repeat) {
        _playlist.setRepeat(repeat);
        showRepeat();
        say(repeat == Playlist::Repeat::Off   ? "Repeat: off"
            : repeat == Playlist::Repeat::All ? "Repeat: all"
                                              : "Repeat: this one");
        changed(false);
        return;
}

void PlayerWindow::showRepeat() {
        const Playlist::Repeat repeat = _playlist.repeat();
        _repeatMenu.clear();
        (void)_repeatMenu.addItem("&Off", ChooseRepeatOff, repeat == Playlist::Repeat::Off ? MenuLayout::Checked : 0);
        (void)_repeatMenu.addItem("&All", ChooseRepeatAll, repeat == Playlist::Repeat::All ? MenuLayout::Checked : 0);
        (void)_repeatMenu.addItem("&One", ChooseRepeatOne, repeat == Playlist::Repeat::One ? MenuLayout::Checked : 0);
        IconTheme &theme = application().icons();
        _repeatButton->setIcon(theme.find(
                repeat == Playlist::Repeat::One ? IconId::MediaPlaylistRepeatSong : IconId::MediaPlaylistRepeat, 16));
        _repeatButton->setChecked(repeat != Playlist::Repeat::Off);
        return;
}

void PlayerWindow::setShuffled(bool shuffled) {
        if (shuffled) _playlist.setSeed(uint32_t(Clock::monotonicNs()) | 1);
        if (_playlist.setShuffled(shuffled).isError()) {
                say("Out of memory: not shuffled");
                shuffled = false;
        }
        showShuffled();
        say(shuffled ? "Shuffle: on" : "Shuffle: off");
        changed(false);
        return;
}

void PlayerWindow::showShuffled() {
        _shuffleButton->setChecked(_playlist.isShuffled());
        _playbackMenu.clear();
        (void)fillPlaybackMenu();
        return;
}

// ---- Playing

uint32_t PlayerWindow::newTrack() {
        if (++_lastTrack == TrackClock::NoTrack) ++_lastTrack;
        return _lastTrack;
}

PlayerWindow::TrackText *PlayerWindow::textOf(uint32_t track) {
        if (track == 0) return nullptr;
        for (TrackText &t : _texts)
                if (t.track == track) return &t;
        return nullptr;
}

PlayerWindow::TrackText &PlayerWindow::newText(uint32_t track) {
        // The oldest place, never the one shown.
        TrackText *t = &_texts[_nextText];
        if (t->track == _shownTrack && _shownTrack != 0) {
                _nextText = (_nextText + 1) % MaxTrackTexts;
                t = &_texts[_nextText];
        }
        _nextText = (_nextText + 1) % MaxTrackTexts;
        t->track = track;
        t->title.clear();
        t->artist.clear();
        t->album.clear();
        t->format[0] = '\0';
        t->frames = Decoder::UnknownFrames;
        t->rateHz = 0;
        t->estimated = false;
        return *t;
}

void PlayerWindow::playEntry(size_t index, Player::Start how) {
        if (index >= _playlist.count()) return;
        const uint32_t track = newTrack();
        TrackText     &t = newText(track);
        (void)t.path.assign(_playlist.path(index));
        (void)t.title.assign(fileName(t.path.view()));
        // The entry playing when it last quit: from where it was, once.
        const uint64_t startMs = index == _resumeEntry ? _resumeMs : 0;
        _resumeEntry = Playlist::NoEntry;
        const Error err = _player.play(track, t.path.view(), startMs, how);
        if (err.isError()) {
                say("The player is busy: try again");
                return;
        }
        _requested = track;
        _seeking = false;
        if (how == Player::Start::Now) _mode = Mode::Playing;
        showMode();
        showList(false);
        _list->showRow(index);
        return;
}

void PlayerWindow::playPause() {
        switch (_mode) {
                case Mode::Playing:
                        if (_player.pause(true).isOk()) {
                                showPosition(); // where it stopped: the timer stops
                                _mode = Mode::Paused;
                        }
                        break;
                case Mode::Paused:
                        if (_player.pause(false).isOk()) _mode = Mode::Playing;
                        break;
                case Mode::Stopped: {
                        if (_playlist.count() == 0) {
                                (void)_openDialog.open();
                                return;
                        }
                        // The chosen row, else the current entry, else the first in the order.
                        size_t index = _playlist.current();
                        if (index == Playlist::NoEntry) index = _playlist.follow();
                        if (index == Playlist::NoEntry) return;
                        _failedInRow = 0;
                        playEntry(index, Player::Start::Now);
                        return;
                }
        }
        showMode();
        showList(false);
        return;
}

void PlayerWindow::stopPlaying() {
        _resumeEntry = Playlist::NoEntry; // stopped: from the start next time
        if (_mode == Mode::Stopped) return;
        const uint32_t track = newTrack();
        if (_player.stop(track).isError()) return;
        _requested = track;
        _mode = Mode::Stopped;
        _clock.clear();
        _seeking = false;
        showMode();
        showPosition();
        showList(false);
        return;
}

void PlayerWindow::next() {
        if (_playlist.count() == 0) return;
        // The next one already queued (the last moments of a track): it, now.
        const bool   ahead = _mode != Mode::Stopped && _clock.shown() != _requested && _clock.shown() != 0;
        const size_t index = ahead ? _playlist.current() : _playlist.next();
        if (_mode == Mode::Stopped) {
                showList(false);
                return;
        }
        if (index == Playlist::NoEntry) {
                stopPlaying();
                return;
        }
        _failedInRow = 0;
        playEntry(index, Player::Start::Now);
        return;
}

void PlayerWindow::previous() {
        if (_playlist.count() == 0) return;
        const bool ahead = _mode != Mode::Stopped && _clock.shown() != _requested && _clock.shown() != 0;
        if (_mode != Mode::Stopped && !ahead && positionMs() > RestartMs) {
                seekTo(0);
                return;
        }
        size_t index = _playlist.previous();
        if (index == Playlist::NoEntry) index = _playlist.current();
        if (_mode == Mode::Stopped) {
                showList(false);
                return;
        }
        if (index == Playlist::NoEntry) return;
        _failedInRow = 0;
        playEntry(index, Player::Start::Now);
        return;
}

void PlayerWindow::seekBy(int64_t ms) {
        if (_mode == Mode::Stopped) return;
        const int64_t to = int64_t(_seeking ? _seekMs : positionMs()) + ms;
        seekTo(to > 0 ? uint64_t(to) : 0);
        return;
}

void PlayerWindow::seekTo(uint64_t ms) {
        // Only in the track heard, and once the thread has it (not in a gapless change).
        TrackText *t = textOf(_requested);
        if (_mode == Mode::Stopped || t == nullptr || _clock.shown() != _requested) return;
        if (t->frames != Decoder::UnknownFrames && t->rateHz != 0) {
                const uint64_t length = t->frames * 1000 / t->rateHz;
                if (ms >= length) ms = length > 1000 ? length - 1000 : 0;
        }
        if (_player.play(_requested, t->path.view(), ms, Player::Start::Now).isError()) return;
        _seeking = true;
        _seekMs = ms;
        showPosition();
        return;
}

void PlayerWindow::setMuted(bool muted) {
        _muted = muted;
        applyGain();
        changed(false);
        return;
}

void PlayerWindow::applyGain() {
        _player.setGain(_muted ? AudioFile::MutedCentiDb : _volume->value() * 100);
        showVolume();
        return;
}

void PlayerWindow::volumeChanged(void *arg, Slider &slider, int32_t value) {
        auto *self = static_cast<PlayerWindow *>(arg);
        (void)slider;
        (void)value;
        self->applyGain();
        self->changed(false);
        return;
}

void PlayerWindow::showVolume() {
        const int32_t db = _volume->value();
        const IconId  id = _muted         ? IconId::AudioVolumeMuted
                           : db <= LowDb  ? IconId::AudioVolumeLow
                           : db <= HighDb ? IconId::AudioVolumeMedium
                                          : IconId::AudioVolumeHigh;
        _muteButton->setIcon(application().icons().find(id, 16));
        _muteButton->setChecked(_muted);
        static_cast<VolumeSlider *>(_volume)->setTip(db, _muted);
        application().toolTipChanged(_volume);
        return;
}

// ---- The thread's reports

void PlayerWindow::reportWaiting(void *arg) {
        auto *self = static_cast<PlayerWindow *>(arg);
        if (self->_reportPosted.exchange(true, std::memory_order_acq_rel)) return;
        if (self->application().post(reportsDue, self).isError())
                self->_reportPosted.store(false, std::memory_order_release); // the tick takes them
        return;
}

void PlayerWindow::reportsDue(void *arg) {
        auto *self = static_cast<PlayerWindow *>(arg);
        self->_reportPosted.store(false, std::memory_order_release);
        self->takeReports();
        return;
}

void PlayerWindow::takeReports() {
        Player::Report r;
        while (_player.take(r)) {
                if (r.track != _requested) continue; // from before the last request
                switch (r.event) {
                        case Player::Event::Started: started(r); break;
                        case Player::Event::Ended: ended(); break;
                        case Player::Event::Failed: failed(r); break;
                        case Player::Event::Stopped:
                                _mode = Mode::Stopped;
                                _clock.clear();
                                _seeking = false;
                                showMode();
                                showPosition();
                                showList(false);
                                break;
                }
        }
        return;
}

void PlayerWindow::started(const Player::Report &r) {
        TrackText *t = textOf(r.track);
        if (t == nullptr) return;
        const bool first = t->rateHz == 0; // not a seek
        if (r.title[0] != '\0') (void)t->title.assign(StringView(r.title));
        (void)t->artist.assign(StringView(r.artist));
        (void)t->album.assign(StringView(r.album));
        formatLine(r.info, t->format, sizeof(t->format));
        t->frames = r.info.frames;
        t->rateHz = r.info.rateHz;
        t->estimated = r.info.estimated;
        _clock.start(r.track, r.streamFrame, r.frame, r.fresh);
        _seeking = false;
        _failedInRow = 0;
        if (first) {
                // What the file says of itself, in the list.
                const size_t entry = _playlist.current();
                if (entry != Playlist::NoEntry && _playlist.path(entry) == t->path.view()) {
                        Playlist::Info info;
                        info.title = r.title[0] != '\0' ? StringView(r.title) : StringView();
                        info.artist = StringView(r.artist);
                        info.album = StringView(r.album);
                        info.lengthMs = r.info.frames == Decoder::UnknownFrames || r.info.rateHz == 0
                                                ? 0
                                                : uint32_t(r.info.frames * 1000 / r.info.rateHz);
                        info.estimated = r.info.estimated;
                        info.known = true;
                        info.format = StringView(formatName(r.info.codec));
                        (void)_playlist.setInfo(entry, info);
                        _playlist.setError(entry, Error::Ok);
                        _playlistUnsaved = _restored; // written with the next change, or at quitting
                }
                changed(false); // the entry playing
        }
        if (_mode == Mode::Stopped) _mode = Mode::Playing;
        showMode();
        showPosition();
        showList(false);
        if (first && !isShown()) tellNowPlaying(*t); // hidden: said by the tray
        return;
}

// "Now playing" (devplan/notifications.md §3.5): the track, its artist and album; the last one
// replaced, quiet, not kept; a click shows the player, its buttons are the player's.
void PlayerWindow::tellNowPlaying(const TrackText &text) {
        Notification n;
        const auto   put = [](char *out, size_t size, StringView v) {
                const size_t len = v.size() < size - 1 ? v.size() : size - 1;
                std::memcpy(out, v.data(), len);
                out[len] = '\0';
        };
        put(n.title, sizeof(n.title), text.title.view());
        String body(MemSpace::Psram);
        if (!text.artist.isEmpty()) (void)body.append(text.artist.view());
        if (!text.album.isEmpty()) {
                if (!body.isEmpty()) (void)body.append('\n');
                (void)body.append(text.album.view());
        }
        put(n.body, sizeof(n.body), body.view());
        put(n.icon, sizeof(n.icon), StringView("multimedia-player"));
        n.urgency = Notification::Urgency::Low;
        n.flags = Notification::Silent | Notification::Transient;
        n.actionCount = 3;
        put(n.actions[0], sizeof(n.actions[0]), StringView("Previous"));
        put(n.actions[1], sizeof(n.actions[1]), StringView(isPaused() ? "Play" : "Pause"));
        put(n.actions[2], sizeof(n.actions[2]), StringView("Next"));
        n.replaces = _nowPlaying;
        Result<uint32_t> id = _notifier.show(n);
        if (id.isError() && n.replaces != 0) { // the last one went meanwhile: a new one
                n.replaces = 0;
                id = _notifier.show(n);
        }
        _nowPlaying = id.isOk() ? id.value() : 0;
        return;
}

void PlayerWindow::nowPlayingTold(void *arg, Notifier &, uint32_t id, Notifier::Event event, uint32_t action) {
        auto *self = static_cast<PlayerWindow *>(arg);
        if (id != self->_nowPlaying || event != Notifier::Event::Activated) return;
        switch (action) {
                case 0: self->showPlayer(); break;
                case 1: self->previous(); break;
                case 2: self->playPause(); break;
                case 3: self->next(); break;
                default: break;
        }
        return;
}

void PlayerWindow::ended() {
        // The next one goes on after it, gapless where it can.
        const size_t index = _playlist.follow();
        if (index == Playlist::NoEntry) {
                const uint32_t track = newTrack();
                if (_player.finish(track).isOk()) _requested = track;
                showList(false);
                return;
        }
        playEntry(index, Player::Start::Follow);
        return;
}

void PlayerWindow::failed(const Player::Report &r) {
        const size_t entry = _playlist.current();
        Error        error = r.error;
        if (r.isMidi && error == Error::NotExist) error = Error::NotFound; // the file is there: its font isn't
        TrackText *t = textOf(r.track);
        if (entry != Playlist::NoEntry && t != nullptr && _playlist.path(entry) == t->path.view())
                _playlist.setError(entry, error);
        char             text[192];
        const StringView name = t != nullptr ? fileName(t->path.view()) : StringView();
        if (error == Error::CorruptData && r.frame > 0 && r.info.rateHz != 0) {
                char at[TimeText::MaxText];
                std::snprintf(text, sizeof(text), "%.*s: damaged at %s", int(name.size()), name.data(),
                              TimeText::format(r.frame * 1000 / r.info.rateHz, Span<char>(at)));
        } else {
                std::snprintf(text, sizeof(text), "%.*s: %s", int(name.size()), name.data(), errorText(error));
        }
        say(StringView(text));
        // The device taken by another program, or every entry failing in a row: stopped.
        if (error == Error::Busy || ++_failedInRow >= _playlist.count()) {
                const uint32_t track = newTrack();
                if (_player.finish(track).isOk()) _requested = track;
                showList(false);
                return;
        }
        const size_t index = _playlist.next(); // Repeat One or not: never the same again
        if (index == Playlist::NoEntry) {
                const uint32_t track = newTrack();
                if (_player.finish(track).isOk()) _requested = track;
                showList(false);
                return;
        }
        playEntry(index, Player::Start::Follow);
        return;
}

const char *PlayerWindow::errorText(Error error) {
        switch (error.code()) {
                case Error::Invalid: return "Not a sound file this player knows";
                case Error::NotSupported: return "A sound file in a form this player doesn't play";
                case Error::CorruptData: return "The file is damaged";
                case Error::NotExist: return "Not found";
                case Error::NotFound: return "No SoundFont: install one, or set synth/font";
                case Error::Busy: return "The sound device is in use (exclusive mode)";
                case Error::NoMem: return "Out of memory";
                case Error::NoMedia: return "The card was taken out";
                default: return error.desc();
        }
}

// ---- Where it is

void PlayerWindow::tick(void *arg) {
        auto *self = static_cast<PlayerWindow *>(arg);
        self->takeReports(); // any a full queue kept from being posted
        self->showPosition();
        return;
}

uint64_t PlayerWindow::positionMs() {
        const AudioFile::Status s = _player.status();
        TrackText              *heardText = textOf(_clock.shown());
        TrackClock::Sample      sample;
        sample.playedFrames = s.playedFrames;
        sample.timestampNs = s.timestampNs;
        sample.lagFrames = s.delayFrames > s.queuedFrames ? s.delayFrames - s.queuedFrames : 0;
        sample.rateHz = heardText != nullptr ? heardText->rateHz : 0;
        sample.playing = s.state == AudioFile::State::Playing || s.state == AudioFile::State::Draining;
        const TrackClock::Now now = _clock.at(TrackClock::heard(sample, Clock::monotonicNs()));
        if (now.track != _shownTrack) {
                _shownTrack = now.track;
                if (now.track != TrackClock::NoTrack) showTrack(textOf(now.track));
        }
        const TrackText *t = textOf(now.track);
        if (t == nullptr || t->rateHz == 0) return 0;
        uint64_t ms = now.frame * 1000 / t->rateHz;
        if (t->frames != Decoder::UnknownFrames) {
                const uint64_t length = t->frames * 1000 / t->rateHz;
                if (ms > length) ms = length;
        }
        return ms;
}

void PlayerWindow::showPosition() {
        char             elapsed[TimeText::MaxText];
        char             left[TimeText::MaxText + 2];
        const TrackText *t = nullptr;
        uint64_t         ms = 0;
        if (_mode != Mode::Stopped) {
                ms = positionMs();
                t = textOf(_shownTrack);
                if (_seeking && _shownTrack == _requested) ms = _seekMs;
        }
        const bool     known = t != nullptr && t->rateHz != 0 && t->frames != Decoder::UnknownFrames;
        const uint64_t length = known ? t->frames * 1000 / t->rateHz : 0;
        _elapsed->setText(StringView(TimeText::format(ms, Span<char>(elapsed))));
        if (known) {
                char time[TimeText::MaxText];
                TimeText::format(length > ms ? length - ms : 0, Span<char>(time));
                std::snprintf(left, sizeof(left), "-%s%s", t->estimated ? "~" : "", time);
                _left->setText(StringView(left));
        } else {
                _left->setText(StringView());
        }
        _seek->setEnabled(known);
        if (!_seek->isDragging()) {
                _seek->setRange(0, known ? int32_t(length / 1000) : 0);
                _seek->setValue(int64_t(ms / 1000));
        }
        showTrayTip(ms);
        return;
}

void PlayerWindow::seekChanged(void *arg, Slider &slider, int32_t value) {
        auto *self = static_cast<PlayerWindow *>(arg);
        if (slider.isDragging()) {
                // Where it would go.
                char elapsed[TimeText::MaxText];
                self->_elapsed->setText(StringView(TimeText::format(uint64_t(value) * 1000, Span<char>(elapsed))));
                return;
        }
        self->seekTo(uint64_t(value) * 1000); // a key or the wheel
        return;
}

void PlayerWindow::seekReleased(void *arg, Slider &slider, int32_t value) {
        (void)slider;
        static_cast<PlayerWindow *>(arg)->seekTo(uint64_t(value) * 1000);
        return;
}

void PlayerWindow::showTrack(const TrackText *t) {
        if (t == nullptr) {
                _title->setText("Nothing playing");
                _subtitle->setText(StringView());
                _format->setText(StringView());
                setTitle(AppTitle);
                return;
        }
        _title->setText(t->title.view());
        char sub[2 * Player::MaxTag + 8];
        if (!t->artist.isEmpty() && !t->album.isEmpty()) {
                std::snprintf(sub, sizeof(sub), "%s \xe2\x80\x94 %s", t->artist.cStr(), t->album.cStr());
        } else {
                std::snprintf(sub, sizeof(sub), "%s", !t->artist.isEmpty() ? t->artist.cStr() : t->album.cStr());
        }
        _subtitle->setText(StringView(sub));
        char line[96];
        if (t->frames != Decoder::UnknownFrames && t->rateHz != 0) {
                char time[TimeText::MaxText];
                std::snprintf(line, sizeof(line), "%s \xc2\xb7 %s%s", t->format, t->estimated ? "~" : "",
                              TimeText::format(t->frames * 1000 / t->rateHz, Span<char>(time)));
        } else {
                std::snprintf(line, sizeof(line), "%s", t->format);
        }
        _format->setText(StringView(line));
        char title[96];
        std::snprintf(title, sizeof(title), "%.60s \xe2\x80\x94 %s", t->title.cStr(), AppTitle);
        setTitle(StringView(title));
        return;
}

void PlayerWindow::showMode() {
        IconTheme &theme = application().icons();
        _playButton->setIcon(
                theme.find(_mode == Mode::Playing ? IconId::MediaPlaybackPause : IconId::MediaPlaybackStart, 16));
        if (_messageTimer < 0)
                _status->setText(0, _mode == Mode::Playing  ? StringView("Playing")
                                    : _mode == Mode::Paused ? StringView("Paused")
                                                            : StringView("Stopped"));
        // The time moves on only while playing: no timer otherwise.
        const bool tick = _mode == Mode::Playing;
        if (tick && _timer < 0) {
                auto [id, err] = application().startTimer(TickMs, true, PlayerWindow::tick, this);
                _timer = err.isOk() ? id : -1;
        } else if (!tick && _timer >= 0) {
                application().stopTimer(_timer);
                _timer = -1;
        }
        if (_mode == Mode::Stopped) {
                _shownTrack = 0;
                if (_playlist.current() == Playlist::NoEntry) showTrack(nullptr);
        }
        return;
}

void PlayerWindow::say(StringView text) {
        _status->setText(0, text);
        if (_messageTimer >= 0) application().stopTimer(_messageTimer);
        auto [id, err] = application().startTimer(MessageMs, false, messageDone, this);
        _messageTimer = err.isOk() ? id : -1;
        return;
}

void PlayerWindow::messageDone(void *arg) {
        auto *self = static_cast<PlayerWindow *>(arg);
        self->_messageTimer = -1;
        self->showMode();
        return;
}

// ---- Adding files

void PlayerWindow::filesChosen(void *arg, FileDialog &dialog, int result) {
        auto *self = static_cast<PlayerWindow *>(arg);
        if (result != FileDialog::Accepted || dialog.count() == 0) return;
        List<StringView> paths(MemSpace::Psram);
        for (size_t i = 0; i < dialog.count(); ++i) (void)paths.append(dialog.path(i));
        // The folder the dialogs open in next: this one (a folder chosen: the one it is in).
        String           folder(MemSpace::Psram);
        const StringView chosen = &dialog == &self->_folderDialog ? dialog.path(0) : dialog.folder();
        const size_t     slash = chosen.lastIndexOf('/');
        if (&dialog != &self->_folderDialog)
                (void)folder.assign(chosen);
        else
                (void)folder.assign(slash == StringView::NPos || slash == 0 ? StringView("/") : chosen.left(slash));
        for (FileDialog *d :
             {&self->_openDialog, &self->_addDialog, &self->_folderDialog, &self->_listDialog, &self->_saveDialog})
                if (d != &dialog) (void)d->setFolder(folder.view());
        (void)self->_lastFolder.assign(folder.view());
        self->addPaths(paths.span(),
                       &dialog == &self->_openDialog || &dialog == &self->_listDialog ? Request::Open : Request::Add);
        self->changed(false);
        return;
}

void PlayerWindow::addPaths(Span<const StringView> paths, Request how) {
        if (how == Request::Play) {
                playPaths(paths);
                return;
        }
        const bool replace = how == Request::Open;
        if (replace) {
                if (_mode != Mode::Stopped) stopPlaying();
                _playlist.clear();
                _resumeEntry = Playlist::NoEntry;
        }
        const size_t before = _playlist.count();
        _addedSome = false;
        for (const StringView p : paths) addPath(p, 0);
        showList(true);
        const size_t added = _playlist.count() - before;
        if (added > 0 || replace) changed(true);
        char         text[64];
        std::snprintf(text, sizeof(text), "%zu file%s added", added, added == 1 ? "" : "s");
        if (!replace) say(StringView(text));
        if (replace && _playlist.count() > 0) {
                _failedInRow = 0;
                const size_t index = _playlist.follow();
                if (index != Playlist::NoEntry) playEntry(index, Player::Start::Now);
        } else if (replace) {
                say("Nothing to play there");
        }
        return;
}

// The file browser's open (`mplayer -p`): a sound file already in the list is played there, one
// that isn't is added at its end; folders and playlists are added. The first of them plays, from
// its start.
void PlayerWindow::playPaths(Span<const StringView> paths) {
        const Vfs::Context context{_cwd.view(), nullptr};
        const size_t       before = _playlist.count();
        size_t             target = Playlist::NoEntry;
        _addedSome = false;
        for (const StringView p : paths) {
                auto [info, err] = Vfs::stat(p, context);
                if (err.isOk() && info.type != FileType::Directory && !isPlaylistName(p)) {
                        size_t at = Playlist::NoEntry;
                        for (size_t i = 0; i < _playlist.count() && at == Playlist::NoEntry; ++i)
                                if (_playlist.path(i) == p) at = i;
                        if (at == Playlist::NoEntry && appendEntry(p).isOk()) at = _playlist.count() - 1;
                        if (target == Playlist::NoEntry) target = at;
                        continue;
                }
                const size_t was = _playlist.count();
                addPath(p, 0);
                if (target == Playlist::NoEntry && _playlist.count() > was) target = was;
        }
        showList(true);
        if (_playlist.count() > before) changed(true);
        if (target == Playlist::NoEntry) {
                say("Nothing to play there");
                return;
        }
        _playlist.setCurrent(target);
        _failedInRow = 0;
        _resumeEntry = Playlist::NoEntry; // from its start
        playEntry(target, Player::Start::Now);
        _list->resetTo(Span<const size_t>(&target, 1), int(target));
        _list->showRow(target);
        return;
}

Error PlayerWindow::appendEntry(StringView path) {
        const Error err = _playlist.append(path);
        if (err == Error::Full) say("The playlist is full (10,000 files)");
        if (err.isOk()) _addedSome = true;
        return err;
}

void PlayerWindow::addPath(StringView path, size_t depth) {
        const Vfs::Context context{_cwd.view(), nullptr};
        auto [info, err] = Vfs::stat(path, context);
        if (err.isError()) {
                char text[160];
                std::snprintf(text, sizeof(text), "%.*s: not found", int(path.size()), path.data());
                say(StringView(text));
                return;
        }
        if (info.type == FileType::Directory) {
                addFolder(path, depth);
        } else if (isPlaylistName(path)) {
                addPlaylistFile(path);
        } else {
                (void)appendEntry(path);
        }
        return;
}

void PlayerWindow::addFolder(StringView path, size_t depth) {
        const Vfs::Context context{_cwd.view(), nullptr};
        auto [dir, err] = Vfs::openDirectory(path, context);
        if (err.isError()) return;
        // Its names in one arena, sorted: the files, then the folders within.
        List<char>     names(MemSpace::Psram);
        List<uint32_t> files(MemSpace::Psram);
        List<uint32_t> folders(MemSpace::Psram);
        for (;;) {
                auto [entry, nerr] = dir.next();
                if (nerr.isError()) break;
                const StringView name = entry.name.view();
                if (name.isEmpty() || name.startsWith('.')) continue;
                const bool folder = entry.type == FileType::Directory;
                if (!folder && !isSoundName(name)) continue;
                const uint32_t at = uint32_t(names.size());
                bool           ok = true;
                for (const char c : name) ok = ok && names.append(c).isOk();
                ok = ok && names.append('\0').isOk();
                ok = ok && (folder ? folders.append(at) : files.append(at)).isOk();
                if (!ok) break;
        }
        dir.close();
        const auto nameAt = [&names](uint32_t at) {
                return StringView(names.data() + at);
        };
        const auto less = [&nameAt](uint32_t a, uint32_t b) {
                return compareNoCase(nameAt(a), nameAt(b)) < 0;
        };
        std::sort(files.data(), files.data() + files.size(), less);
        std::sort(folders.data(), folders.data() + folders.size(), less);
        String full(MemSpace::Psram);
        for (const uint32_t at : files) {
                full.clear();
                if (full.append(path).isError() || (!path.endsWith('/') && full.append('/').isError()) ||
                    full.append(nameAt(at)).isError())
                        return;
                if (appendEntry(full.view()).isError()) return;
        }
        if (depth + 1 >= MaxDepth) return;
        for (const uint32_t at : folders) {
                full.clear();
                if (full.append(path).isError() || (!path.endsWith('/') && full.append('/').isError()) ||
                    full.append(nameAt(at)).isError())
                        return;
                addFolder(full.view(), depth + 1);
                if (_playlist.count() >= Playlist::MaxEntries) return;
        }
        return;
}

void PlayerWindow::addPlaylistFile(StringView path) {
        String      text(MemSpace::Psram);
        const Error err = readText(path, _cwd.view(), MaxPlaylistBytes, text);
        if (err.isError()) {
                char msg[160];
                std::snprintf(msg, sizeof(msg), "%.*s: %s", int(fileName(path).size()), fileName(path).data(),
                              err == Error::OutOfRange ? "too large a playlist" : err.desc());
                say(StringView(msg));
                return;
        }
        const size_t     slash = path.lastIndexOf('/');
        const StringView folder = slash == StringView::NPos || slash == 0 ? StringView("/") : path.left(slash);
        auto [result, merr] = M3u::read(text.view(), folder, _playlist);
        if (merr == Error::Full) say("The playlist is full (10,000 files)");
        if (result.added > 0) _addedSome = true;
        return;
}

// ---- Kept between runs (devplan/mediaplayer.md §6f)

Error PlayerWindow::readText(StringView path, StringView cwd, size_t maxBytes, String &out) {
        out.clear();
        auto [file, err] = Vfs::open(path, Vfs::Read, Vfs::Context{cwd, nullptr});
        while (err.isOk()) {
                char buf[512];
                auto [n, rerr] =
                        file->read(WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)), WaitForever);
                if (rerr == Error::EndOfFile || (rerr.isOk() && n == 0)) break;
                err = rerr;
                if (err.isOk() && out.size() + n > maxBytes) err = Error::OutOfRange;
                if (err.isOk()) err = out.append(StringView(buf, n));
        }
        return err;
}

// Written beside it first, then put in its place: a card taken out, or the power going, while
// writing leaves the old one or the new one (the new one's name read if the old one went).
Error PlayerWindow::writeFile(StringView path, StringView cwd, StringView text) {
        const Vfs::Context context{cwd, nullptr};
        String             temporary(MemSpace::Psram);
        Error              err = temporary.appendf("%.*s.new", int(path.size()), path.data());
        if (err.isError()) return err;
        {
                auto [file, oerr] = Vfs::open(temporary.view(), Vfs::Write | Vfs::Create | Vfs::Truncate, context);
                if (oerr.isError()) return oerr;
                auto [n, werr] =
                        file->write(ByteSpan(reinterpret_cast<const uint8_t *>(text.data()), text.size()), WaitForever);
                err = werr.isError() ? werr : n != text.size() ? Error(Error::IoError) : Error();
        }
        if (err.isOk()) {
                err = Vfs::remove(path, context);
                if (err == Error::NotExist) err = Error::Ok;
        }
        if (err.isOk()) err = Vfs::rename(temporary.view(), path, context);
        if (err.isError()) (void)Vfs::remove(temporary.view(), context);
        return err;
}

void PlayerWindow::readSettings(StringView cwd, PlayerSettings &out) {
        String path(MemSpace::Psram);
        String text(MemSpace::Psram);
        if (path.appendf("%s/%s", KeptDir, SettingsName).isError()) return;
        Error err = readText(path.view(), cwd, MaxSettingsBytes, text);
        if (err == Error::NotExist && path.append(".new").isOk())
                err = readText(path.view(), cwd, MaxSettingsBytes, text);
        if (err.isOk()) (void)out.parse(text.view());
        return;
}

void PlayerWindow::restore(const PlayerSettings &settings) {
        _volume->setValue(settings.volumeDb);
        _muted = settings.muted;
        applyGain();
        _playlist.setRepeat(settings.repeat);
        showRepeat();
        // The playlist, then its order (shuffled: a new one) and the entry it was at.
        String path(MemSpace::Psram);
        String text(MemSpace::Psram);
        if (path.appendf("%s/%s", KeptDir, PlaylistName).isOk()) {
                Error err = readText(path.view(), _cwd.view(), MaxPlaylistBytes, text);
                if (err == Error::NotExist && path.append(".new").isOk())
                        err = readText(path.view(), _cwd.view(), MaxPlaylistBytes, text);
                if (err.isOk()) (void)M3u::read(text.view(), StringView(KeptDir), _playlist);
        }
        if (settings.shuffled) {
                _playlist.setSeed(uint32_t(Clock::monotonicNs()) | 1);
                (void)_playlist.setShuffled(true);
        }
        showShuffled();
        if (settings.entry < _playlist.count()) {
                _playlist.setCurrent(settings.entry);
                _resumeEntry = settings.entry;
                _resumeMs = settings.positionMs;
                // What Play plays, and from where.
                char             elapsed[TimeText::MaxText];
                const StringView title = _model.text(settings.entry);
                char             line[96];
                std::snprintf(line, sizeof(line), "%.*s", int(title.size()), title.data());
                _title->setText(StringView(line));
                _elapsed->setText(StringView(TimeText::format(_resumeMs, Span<char>(elapsed))));
        }
        if (settings.tab < _tabs->count()) _tabs->setCurrent(settings.tab);
        if (!settings.lastFolder.isEmpty()) {
                (void)_lastFolder.assign(settings.lastFolder.view());
                for (FileDialog *d : {&_openDialog, &_addDialog, &_folderDialog, &_listDialog, &_saveDialog})
                        (void)d->setFolder(_lastFolder.view());
        }
        showList(true);
        if (_resumeEntry != Playlist::NoEntry) {
                const size_t row = _resumeEntry;
                _list->resetTo(Span<const size_t>(&row, 1), int(row));
                _list->showRow(row);
        }
        _restored = true;
        return;
}

void PlayerWindow::changed(bool playlist) {
        if (!_restored) return;
        _settingsUnsaved = true;
        _playlistUnsaved = _playlistUnsaved || playlist;
        if (_saveTimer >= 0) application().stopTimer(_saveTimer); // 2 s after the last
        auto [id, err] = application().startTimer(SaveMs, false, saveDue, this);
        _saveTimer = err.isOk() ? id : -1;
        return;
}

void PlayerWindow::saveDue(void *arg) {
        auto *self = static_cast<PlayerWindow *>(arg);
        self->_saveTimer = -1;
        self->saveNow();
        return;
}

void PlayerWindow::settingsNow(PlayerSettings &out) {
        out.volumeDb = _volume->value();
        out.muted = _muted;
        out.repeat = _playlist.repeat();
        out.shuffled = _playlist.isShuffled();
        // The entry, and where in it: the track heard if it is that entry's, else its start.
        const size_t current = _playlist.current();
        out.entry = current;
        out.positionMs = 0;
        if (current != Playlist::NoEntry && _mode != Mode::Stopped) {
                const TrackText *t = textOf(_requested);
                if (t != nullptr && _clock.shown() == _requested && t->path.view() == _playlist.path(current))
                        out.positionMs = _seeking ? _seekMs : positionMs();
        } else if (current != Playlist::NoEntry && current == _resumeEntry) {
                out.positionMs = _resumeMs; // never played since
        }
        if (_keptWidth != 0 && _keptHeight != 0) {
                out.width = WindowFrame::outerWidth(_keptWidth, WindowFrame::Standard);
                out.height = WindowFrame::outerHeight(_keptHeight, WindowFrame::Standard);
        }
        out.tab = uint32_t(_tabs->current());
        (void)out.lastFolder.assign(_lastFolder.view());
        return;
}

void PlayerWindow::saveNow() {
        if (!_restored) return;
        if (_saveTimer >= 0) {
                application().stopTimer(_saveTimer);
                _saveTimer = -1;
        }
        const Vfs::Context context{_cwd.view(), nullptr};
        (void)Vfs::mkdir("/home/.config", context);
        (void)Vfs::mkdir(StringView(KeptDir), context);
        String path(MemSpace::Psram);
        String text(MemSpace::Psram);
        Error  err = Error::Ok;
        if (_playlistUnsaved) {
                err = M3u::write(_playlist, StringView(KeptDir), text);
                if (err.isOk()) err = path.appendf("%s/%s", KeptDir, PlaylistName);
                if (err.isOk()) err = writeFile(path.view(), _cwd.view(), text.view());
                if (err.isOk()) _playlistUnsaved = false;
        }
        // The settings every time: the position moves on.
        PlayerSettings settings;
        settingsNow(settings);
        path.clear();
        Error serr = settings.write(text);
        if (serr.isOk()) serr = path.appendf("%s/%s", KeptDir, SettingsName);
        if (serr.isOk()) serr = writeFile(path.view(), _cwd.view(), text.view());
        if (serr.isOk()) _settingsUnsaved = false;
        if (err.isError() || serr.isError()) {
                char msg[96];
                std::snprintf(msg, sizeof(msg), "Not kept for next time: %s", (err.isError() ? err : serr).desc());
                say(StringView(msg));
        }
        return;
}

void PlayerWindow::saveChosen(void *arg, FileDialog &dialog, int result) {
        auto *self = static_cast<PlayerWindow *>(arg);
        if (result != FileDialog::Accepted || dialog.count() == 0) return;
        self->savePlaylistAs(dialog.path(0));
        return;
}

void PlayerWindow::savePlaylistAs(StringView path) {
        const size_t     slash = path.lastIndexOf('/');
        const StringView folder = slash == StringView::NPos || slash == 0 ? StringView("/") : path.left(slash);
        String           text(MemSpace::Psram);
        Error            err = M3u::write(_playlist, folder, text);
        if (err.isOk()) err = writeFile(path, _cwd.view(), text.view());
        char             msg[160];
        const StringView name = fileName(path);
        if (err.isOk()) {
                std::snprintf(msg, sizeof(msg), "Saved %.*s", int(name.size()), name.data());
        } else {
                std::snprintf(msg, sizeof(msg), "%.*s: can't save it: %s", int(name.size()), name.data(), err.desc());
        }
        say(StringView(msg));
        if (err.isOk() && _lastFolder.view() != folder) {
                (void)_lastFolder.assign(folder);
                for (FileDialog *d : {&_openDialog, &_addDialog, &_folderDialog, &_listDialog})
                        (void)d->setFolder(folder);
                changed(false);
        }
        return;
}

BMDS_NAMESPACE_END
