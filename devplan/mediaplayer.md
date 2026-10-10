# Media Player

Planned 2026-10-09; its main questions answered the same day (§4), designed in detail the same day
(§6), being built (§9). The user's request
(2026-10-08): after the synth works, "a proper fully featured GUI media player that'll play both
audio files and midi files (and perhaps video files in the future)". It is step 8 of
`devplan/audio.md` §10.

## 1. Goal

- **A GUI application** that plays everything the decoders take (WAV, MP3, Ogg Vorbis, FLAC, and
  MIDI files through the synth), with the controls people expect of a media player.
- **Fully featured,** within reason for this machine: a playlist, seeking, volume, what is
  playing and its tags, repeat and shuffle, keys, opening from the file browser.
- **Video later:** nothing in its design assumes media is only sound.
- **Like every built-in application:** only the public GUI API and the kernel's public classes;
  no OS code knows it (`builtin-apps-not-special`).

## 2. What exists to build on (checked 2026-10-09)

- **Decoding:** `Decoder` (`bmds_audio`): open by content, the format chosen, `decode()`, exact
  `seek()`, the length (estimated for MP3 without a Xing frame), tags (`TITLE`, `ARTIST`, `ALBUM`,
  `TRACKNUMBER`, `DATE`...). MIDI files are a format (`Codec::Midi`), played on `synth/font`; their
  seek replays controllers and programs, not notes.
- **Playing:** an `AudioFile` stream (`/dev/dsp`): a ring, start, pause, drain, drop, its gain,
  and its status (frames written and played, with the timestamp of the last; the device's rate).
  `play` does this already, in the shell.
- **The toolkit** (`bmds_gui`): windows, a menu bar, tool and status bars, buttons, `Slider`,
  `DetailsView` (a sortable list), `IconView`, `TreeView`, `ScrollArea`, `Dialog`, `MessageBox`,
  `LineEdit`, tooltips, mnemonics and focus, `GuiApplication::watchConfig()`.
- **Not there:** an Open dialog (the file browser's design, `filebrowser.md` FB7, put its views in
  the toolkit so that one could be made: `FolderModel` in `bmds_files`).
- **Files open by type** (`mimeapps.list`): `audio/*` opens with `play.desktop` today; the player
  would take its place.
- **The tray** has a volume control (`audio.md` §17); the panel has a window list and a launcher.

## 3. A shape to start from (agreed; §6 is the design)

- **`/bin/player`** (or another name), a built-in GUI application with a launcher entry, opened
  for `audio/*` from the file browser and with files on its command line.
- **The window:**
  - a menu bar (File: Open, Open Folder, Add, Save Playlist, Quit; Playback; View; Help);
  - what is playing: title, artist, album (or the file's name), the format, rate and length;
  - a position slider with the time played and left, and transport buttons (previous, play or
    pause, stop, next), repeat and shuffle toggles, and a volume slider (the stream's gain);
  - the playlist, a `DetailsView` (number, title, artist, length, format), reordered and edited
    in place;
  - a status bar (buffering, errors, the synth's voices for a MIDI file).
- **Playing on its own thread:** decoding into the stream's ring ahead of the hand; the position
  from the stream's played frames (the device's own clock, §9's rule), never from a timer; a seek
  is the decoder's seek and the stream's drop; the next file starts as the last drains, so
  albums play without a gap where the formats allow.
- **Errors said plainly:** a damaged file, a missing font for MIDI, a busy device, each in the
  status bar and skipped, not a stopped player.
- **Media later:** the playing side as a `MediaSource` (its streams: sound, later pictures) so
  that video adds a source and a view, not a second player.

## 4. Decisions (the user, 2026-10-09)

- **Its name:** `mplayer` (`/bin/mplayer`; "Media Player" in the launcher). (The free MPlayer
  project's program has the same name; this one is BMDS's own and shares nothing with it.)
- **Features, beyond the basics** (play, pause, stop, seek, volume, a playlist, tags, repeat and
  shuffle), all four offered:
  - **saved playlists:** the playlist kept between runs, and `.m3u` saved and loaded;
  - **a library by tags:** folders on the card scanned, browsed by artist and album;
  - **a visualizer:** a spectrum or a level meter while playing;
  - **a MIDI view:** for MIDI files, the 16 channels' instruments and activity, the tempo, and the
    SoundFont chosen.
- **A toolkit Open dialog, now:** a reusable Open and Save dialog in `bmds_gui`, on the file
  browser's models and views (`bmds_files`), for every application.
- **It keeps playing with its window closed:** closing hides it; a controller in the panel's
  tray brings it back or stops it.

## 5. What the decisions bring with them (designed in §6)

- **The tray controller** can't be the panel knowing `mplayer` (`builtin-apps-not-special`): it
  needs a general way for an application to put an item in the tray (an icon, a tooltip, a menu,
  a click), as freedesktop.org's StatusNotifierItem does, in the window manager's protocol.
  Media keys (`hotkeys.md`) would reach the player through the same kind of mechanism.
- **The MIDI view** needs the synth's state per channel (program, volume, notes sounding) from the
  decoder that plays the file: an interface on `Decoder` for what a format can tell while playing,
  or the player playing MIDI files through `Synth` itself.
- **The visualizer** needs the samples as they play (an FFT of the period leaving the device, or
  of what the player wrote, aligned by the stream's position), at a cost to measure.
- **The library** reads every file's tags (opening a decoder per file): a scan on its own thread,
  cached on the card, updated by the VFS's directory watches.
- **The playlist kept between runs:** in the user's home (`/home/.config/mplayer/`).
- **Its volume** is its stream's gain, so the planned mixer window (`audio.md` §17) shows and sets
  the same level.

## 6. Design (2026-10-09)

### 6a. Where it lives

- **`components/bmds_mplayer`:** `/bin/mplayer`, a built-in program (`BMDS_PROGRAM_ICONS`,
  `WHOLE_ARCHIVE`, as the terminal), on the public toolkit and the kernel's public classes only.
- **Its pure parts** (no toolkit, no IDF; host-tested from `tests/host`): the playlist and its
  orders, M3U, the library's index, the MIDI channel model, the spectrum's bands.
- **What other programs can use goes to the shared components:**
  - the Open and Save dialog, the drop-down list, tabs, tray items: `bmds_gui` (§6d);
  - the FFT: `bmds_core` (portable, for the SDK's synth library later too);
  - a decoder's information without decoding, MIDI files' events as they play: `bmds_audio` and
    `bmds_synth` (§6f, §6h).

### 6b. Playing

- **Two threads:**
  - the loop (`GuiApplication`): the window, the tray item, the dialogs;
  - **the playback thread** (`Player`, 8 KB stack in PSRAM: the decoders keep their state off the
    stack): opens files, decodes, writes to the stream.
- **The stream:** `/dev/dsp` (the default device) opened for writing, configured from the
  decoder's rate and channels, in `F32Le` (MIDI's own; the others converted by the decoder).
- **Commands** (the loop to the thread): play an entry, pause, resume, stop, seek, next, previous,
  set the gain. In a small queue under a lock; the thread is woken by a semaphore.
  - **Writes are a period at a time** (about 20 ms), so a command waits one period at most.
  - **A seek or a stop** also calls `drop()`, which wakes a writer waiting for room
    (`AudioFile` is thread-safe), and the ring's old sound goes at once.
  - **Pause** is the stream's own (`setPaused()`): the ring keeps its sound, resume is instant.
- **Reports** (the thread to the loop) by `GuiApplication::post()`: a track started (its entry,
  its first frame on the stream), a track ended, an error (the entry, the error), stopped.
- **Where it is:** the stream's status (frames played, the timestamp of the last). Never a
  timer's count (`audio.md` §9: the device's clock).
  - The position in a track: frames played less the track's first frame on the stream, plus the
    frame it was started or sought at, over the track's rate.
  - Between status updates, the timestamp moves it on, so the time and the slider move smoothly.
  - **Redrawn by a timer only while it is shown and playing** (4 a second for the time; the
    visualizer's 30 when shown, §6g); none while paused, stopped or hidden.
- **Gapless:** at a track's end the next one opens at once.
  - **Same rate and channels:** it goes on into the same stream with no drain, so there is no gap.
    Its first frame on the stream is recorded, and the window changes to it when the played
    frames reach it.
  - **Otherwise:** the stream drains, then is configured again (a gap of the ring's length).
- **Volume:** the stream's gain (`setGain()`, centi-dB), −60 to 0 dB and mute, saved in
  `mplayer.conf`. The mixer window (`audio.md` §17) will show the same level.
- **Errors said plainly, then skipped:** in the status bar, the entry marked in the playlist (an
  icon, the error in its tooltip).
  - A file that won't open: `Invalid` "Not a sound file this player knows", `NotSupported`
    "Ogg Opus isn't supported" (the decoder's reason), `CorruptData` "Damaged at 1:23".
  - A MIDI file with no font: "No SoundFont: install one, or choose one in the MIDI view".
  - The device in exclusive use: "The sound device is in use (exclusive mode)", and it stops.
  - Every entry failing in a row stops the player rather than spinning through the list.

### 6c. The window

```
+- Media Player ------------------------------------------------- _ □ x -+
| File  Playback  View  Help                                              |
+------------+------------------------------------------------------------+
| ▌▌ ▌ ▌▌▌▌  | He's a Pirate                                              |
| ▌▌▌▌▌▌▌▌▌  | Klaus Badelt — Pirates of the Caribbean                    |
| (visualizer| MIDI · 48 kHz · stereo · 3:21                              |
+------------+------------------------------------------------------------+
| 0:42  [=========o-----------------------------------------]  -2:39     |
| [|◀][▶‖][■][▶|]  [⟳][⤮]                           🔈 [-------o---]      |
+-------------------------------------------------------------------------+
| [Playlist] [Library] [MIDI]                                             |
| #  Title                    Artist          Length  Format              |
| 1  ▶ He's a Pirate          Klaus Badelt     3:21   MIDI                |
| 2    Axel F                 Harold Faltermeyer 3:00 MIDI                |
+-------------------------------------------------------------------------+
| Playing · 14 voices                                     2 of 12 · 41:07 |
+-------------------------------------------------------------------------+
```

- **Menus:**
  - **File:** Open… (Ctrl+O: replaces the playlist and plays), Add Files…, Add Folder…, Open
    Playlist…, Save Playlist As…, Close (Ctrl+W: hides it while playing), Quit (Ctrl+Q).
  - **Playback:** Play/Pause (Space), Stop, Previous (Ctrl+Left), Next (Ctrl+Right), Back 5 s
    (Left), Forward 5 s (Right), Repeat (Off, All, One), Shuffle.
  - **View:** Playlist, Library, MIDI (the tabs: Ctrl+1 to 3), Visualizer: Spectrum, Levels, Off.
  - **Help:** About.
- **Now playing:** the title (or the file's name), artist and album, the format line.
- **The position:** the time played, a `Slider` (seeks on release; while dragged, the time shows
  where it would go), the time left. A length the decoder estimated is shown with `~`.
- **Transport:** previous, play or pause, stop, next; repeat and shuffle as checkable buttons;
  the volume slider (a tooltip with the dB; the wheel 1 dB a notch).
- **The tabs:** the playlist (a `DetailsView`: number, title, artist, album, length, format; the
  playing entry marked; double click or Return plays; Delete removes; Ctrl+Up and Ctrl+Down move
  the selection; files dropped from the file browser later), the library (§6f), the MIDI view
  (§6h).
- **Status bar:** the state and what's useful about it (buffering, a MIDI file's voices, the last
  error), the entry's number and the playlist's total time.
- **Its icons:** the theme's `media-playback-start` and friends on the buttons;
  `multimedia-player` for the window, the launcher and the tray.

### 6d. Toolkit additions (`bmds_gui`), for every program

- **`FileDialog`:** Open, Open several, Save, Choose Folder.
  - **Layout:** a "Look in" row (Back, Up, the folder's path in a `LineEdit`), places on the left
    (the home folder, the card's partitions, `/boot`), the folder in a `DetailsView` (Details or
    Icons), then "File name" (a `LineEdit`) and "Files of type" (a `ComboBox`) over Open/Save
    and Cancel.
  - **Built on the file browser's parts:** `FolderModel` and `NavigationHistory` (`bmds_files`);
    `FolderItems` and `FileIcons` move from the panel into `bmds_gui` (as `FolderItemModel` and
    `FileIcons`), the file browser using them from there unchanged; directory watches keep the
    folder current while the dialog is open.
  - **Filters:** a list of names and patterns given by the caller ("Media files" = `*.mp3;*.ogg;
    *.flac;*.wav;*.mid;*.midi;*.m3u;*.m3u8`, "All files"), applied to files, never to folders.
  - **Keys:** typing a name selects it; Return on a folder enters it, on a file accepts;
    Backspace goes up; a path typed in the name field goes there.
  - **Save** asks before replacing a file (a `MessageBox`) and adds the filter's extension to a
    name without one.
  - **Not modal,** as `Dialog`: `Finished` gives the result; `paths()` the chosen ones.
  - **Where it starts:** the folder the caller gives (the player: the last one it used).
- **`ComboBox`:** a button showing the chosen item, a `Menu` of the items when pressed; keys Up
  and Down change it, Alt+Down opens it. For the type filter, the repeat mode, the font.
- **`TabWidget`:** a row of tabs over stacked pages; a click or Ctrl+Tab, Ctrl+Shift+Tab, or
  Ctrl+PageDown, Ctrl+PageUp change pages; the hidden pages aren't laid out or painted.
- **`Slider`:** a `Released` callback, and `isDragging()`, for a seek bar that seeks on release.
- **`TrayIcon`** (a program's item in the panel's tray, §6e): an icon, a tooltip, `Activated`
  (a left click) and `ContextMenu` (a right click, at a screen point: the program opens its own
  `Menu` there).
- **Each tried in `/bin/widgets`** before the player uses it, host tests for the pure parts
  (the filter's matching, `ComboBox` and `TabWidget` key handling where pure).

### 6e. The system tray (for every program; the user's, 2026-10-09)

The user asked for "a proper system tray API (perhaps similar to what Qt uses)", with the volume
control moved onto it. So the tray is a general service: Qt's `QSystemTrayIcon` is the model for
the program's side, and freedesktop.org's StatusNotifierItem for what the protocol carries.

- **`TrayIcon`** (`bmds_gui`), a program's item:
  - `setIcon(name)` (a theme name) or `setIcon(const Icon &)` (16 × 16 pixels of its own,
    copied: the volume's muted speaker is drawn by the program); `setToolTip(text)`, changed
    live (the volume's dB follows the wheel);
  - `setContextMenu(Menu *)`: a right click opens it at the point the tray gives;
  - `setActivated(fn, arg)` with a reason, as Qt's: `Trigger` (a click), `DoubleClick`,
    `MiddleClick`, `Context` (a right click, also told when there is a menu);
  - `setScrolled(fn, arg)`: wheel notches (StatusNotifierItem's `Scroll`; Qt has none);
  - `setCategory()`: `Application` (default), `Communications`, `System`, `Hardware`. The tray
    places items by category, then in the order they came: hardware at the right end;
  - `show()`, `hide()`, `isVisible()`;
  - `geometry()`: its place on the screen, so a program can open a popup against it (the
    volume's slider);
  - `isSystemTrayAvailable()` and `setAvailabilityChanged(fn, arg)`;
  - `showMessage(title, text, icon, timeoutMs)` and `setMessageClicked(fn, arg)`: a balloon by
    the icon (Qt's), for notifications (built after the rest, step 10).
- **The protocol: window manager services** (`filebrowser.md` §4h, built):
  - the panel's tray registers the service `tray`; each program with tray icons registers
    `tray.<pid>` for the replies;
  - **a program sends:** Add or Update (its icon's number, its service, the category, an icon
    name or "pixels follow", the tooltip, shown or not), Pixels (16 × 16 ARGB), Remove, and
    Show Message;
  - **the tray sends:** Activated (the reason and the icon's screen rectangle), Scrolled
    (notches and the rectangle), Geometry (the rectangle, when the row moves), Message Clicked;
  - **each message is a struct starting with `uint32_t size`** and a kind; fields are appended,
    never changed (`docs/ABI.md`'s rule, ahead of the GUI API being frozen);
  - **a 16 × 16 icon is 1 KB,** the service queue's largest message now: `ServiceQueue` grows to
    2 KB a message and 8 KB waiting (it allocates only what is queued).
- **Programs and the panel coming and going:** a connection's services go with it. New in the
  window manager: `watchServices(client, fn, arg)`, telling a connection when any service
  appears or goes. The tray drops a gone program's icons; `TrayIcon` adds itself again when
  `tray` comes back (the panel restarted). Each does this on its own; programs write nothing.
- **The panel's tray area:** a new item kind `tray` in `panel/items`, a 24-pixel cell an icon
  (16-pixel icons), tooltips by the toolkit's, the cell sunken while pressed. The default
  becomes `launcher;windows;tray;clock`. A saved value naming `volume` and not `tray` puts the
  tray where `volume` was, so nobody's panel loses its volume control.
- **The volume control stays in the panel's process, as a `TrayIcon`** (the user's, 2026-10-09:
  a program of its own would cost a task, a stack and a connection for nothing). The panel's
  `VolumeItem` stops being a panel item: its code makes a `TrayIcon` and goes through the same
  messages as any other program's icon, so the tray treats it as it treats them, and it is the
  API's first user and test.
  - Category `Hardware`; the icon and tooltip as now (`gui.md` §4s); a click opens the slider's
    popup (`VolumePopup`, unchanged) against `geometry()`; the wheel 1 dB a notch; the context
    menu Mute and Mixer… (`audio.md` §17: Mixer… waits for the mixer window).
  - It is shown whenever the panel has a tray. No autostart is needed for it; one can come when
    a tray program of its own needs starting with the desktop.
- **The player** shows its icon whenever it runs (the user's, 2026-10-09, §6i).

### 6f. The playlist, M3U, the library

- **The playlist** (pure, `Playlist`): entries in one PSRAM text arena (the path; the tags once
  known: title, artist, album, the length, the format), up to 10,000.
  - **Orders:** the list's, and shuffle's, which plays each entry once before any again (a new
    order after all, never starting with the one just played).
  - **Repeat:** off (stops after the last), all, one.
  - **Removing or moving entries** keeps the current entry and the shuffle order right.
  - **Tags filled in the background:** an entry added shows its file's name until the library's
    scanner (below) has read it.
- **M3U** (pure): `#EXTM3U` with `#EXTINF:seconds,Artist - Title`, plain lists, `.m3u8` (UTF-8;
  `.m3u` read as UTF-8 too), CR LF, a BOM, comments; paths relative to the list's folder are
  resolved on reading and written relative when they are under it. URLs are skipped (no network).
- **Kept between runs** in `/home/.config/mplayer/`:
  - `playlist.m3u8`: the playlist;
  - `mplayer.conf` (INI): the volume and mute, repeat, shuffle, the entry and position playing
    at the end, the tab and visualizer shown, the window's size, the last folder, the library's
    folders, the MIDI font (§6h);
  - written 2 s after the last change (as the tray's volume), and on quitting.
- **The library:**
  - **Its folders:** a list in `mplayer.conf`, by default `/home/Music`; Library Folders… in the
    library's tab adds and removes them (a `FileDialog` choosing folders).
  - **The scanner,** a thread of its own at the lowest priority: walks the folders, and for each
    file with a known extension not in the index or changed (its size and time), reads its
    information. Nothing else while playing is slowed: it yields between files.
  - **Information without decoding** (new: `Decoder::probe()`): the format, rate, channels,
    length and tags, without what decoding needs. For a MIDI file this matters most: opening it
    as a decoder loads the SoundFont; probing parses the file for its length and its name
    (track 1's name event) only.
  - **The index:** `/home/.config/mplayer/library.idx`, a line a file (tab-separated: path,
    size, time, length, format, title, artist, album, track, year); read at start, written by
    the scanner after it changes.
  - **Kept current while running** by directory watches on the folders.
  - **The view:** artists, then their albums, in a `TreeView` on the left; the tracks of the
    choice in a `DetailsView`; double click plays a track (its album after it), Add to Playlist
    from the context menu.

### 6g. The visualizer

- **What it shows:** the sound as it is heard (the device's position), not as decoded ahead.
  - The playback thread keeps the last 4,096 frames it wrote (mixed to mono, float: 16 KB) in a
    ring, with the stream frame of its newest.
  - At each of its 30 frames a second (only while shown and playing), the loop takes the 1,024
    frames ending at the played position (from the stream's status) and analyses them.
- **Spectrum:** a Hann window, a real FFT of 1,024 points (`bmds_core`'s `Fft`, float, radix 2;
  host-tested against a plain DFT), 32 bands from 40 Hz to 16 kHz on a log scale, −60 to 0 dB,
  each bar falling 20 dB a second with a peak mark held a second.
- **Levels:** left and right peak and RMS bars, from the same frames before mixing (the ring keeps
  both channels' peaks per 64 frames).
- **Cost, to measure on the board:** the FFT is about 25,000 float operations (estimated
  0.1 ms); the bars a 128 × 80 region. If the FFT shows up, PIE's SIMD (`efficiency-every-trick`).

### 6h. The MIDI view

- **Its contents:** for a MIDI file playing,
  - **16 rows, a channel each:** its number (10 marked drums), the instrument (the font's preset
    name for its bank and program, or the General MIDI name), volume, pan, and a bar of its
    notes' activity (the loudest velocity sounding, falling);
  - **the tempo** (beats per minute), the time signature and the bar and beat;
  - **the font:** a `ComboBox` of the SoundFonts found in `/sd0/p0/share/soundfonts` and the
    folders of the fonts already used, and Other… (a `FileDialog`).
- **Where the state comes from, at the time it is heard:** `MidiDecoder` records what it plays as
  it renders (program, bank, volume, pan and expression changes; note on and off with their
  velocity; tempo and time signature), each with its stream frame, in a ring of 512 the player
  takes after each decode (new: `MidiDecoder::takeEvents()`). The player applies them to a pure
  `MidiChannels` model when the played frames reach them, as the visualizer does. A ring that
  overflows sets a flag and gives a snapshot of every channel instead.
- **A seek** gives a snapshot too: the decoder's seek replays controllers and programs.
- **Changing the font:** the file is opened again on the new font and sought to the position
  (a short break, a load of about 0.7 s the first time). The choice is the player's own (§10):
  the decoder takes a font's path when opened (new: `MidiDecoder::open(source, font)`; the
  shared font list loads it as it loads `synth/font`), an empty one meaning `synth/font`.

### 6i. Hidden, the tray, one player

- **Closing the window while playing hides it** (the window is closed, the program goes on) and
  the tray item stays; **closing it while stopped quits**.
- **The tray item** is shown while the player runs (the user's, 2026-10-09):
  - **tooltip:** "Title — Artist", the time and length, or "Stopped";
  - **left click:** shows the window (where it was) and raises it;
  - **right click:** Play/Pause, Stop, Previous, Next, Show Player, Quit.
- **One player:** it registers the service `mplayer`. `mplayer FILE...` with one running sends
  the files to it (replacing the playlist, or `-a` adding to it) and its window shows; `mplayer`
  alone shows it. Exit status 0 if it took them, 1 with the reason otherwise.
- **Opening from the file browser:** `mplayer.desktop` (`Exec=mplayer %F`, `MimeType=audio/*`
  and `audio/x-mpegurl`); `mimeapps.list` gives `audio/*` to it (`play.desktop` stays, for Open
  With). `.m3u` and `.m3u8` become `audio/x-mpegurl` in the file types' table.
- **Media keys** come with `hotkeys.md`: the player registers `media.play-pause`, `media.next`,
  `media.previous` and `media.stop` then. Until then its keys work in its window.

## 7. Budget (estimates, to be measured)

- **Firmware:** the toolkit's additions 12 to 16 KB (the file dialog most), the player 30 to 40 KB,
  the FFT 2 KB, the tray in the panel and the window manager 3 KB.
- **Running, in PSRAM:** the program's stack 16 KB and the playback thread's 8 KB, the stream's
  ring (the device's), the decoder (MP3 about 25 KB, Vorbis up to 200 KB, MIDI the synth's
  voices with the font shared), the visualizer's ring 16 KB, the playlist about 200 bytes an
  entry, the library's index about 150 bytes a file (1,000 files: 150 KB). Internal RAM: the
  threads' kernel records only.
- **CPU:** decoding as `play` (MP3 a few percent of a core), the visualizer's analysis and bars
  at 30 a second (measured before choosing the default), nothing at all while paused or hidden
  but the decoding.

## 8. ABI impact

- **New interfaces kept once shipped:** `mplayer`'s command line (`[-a] [FILE...]`), the files in
  `/home/.config/mplayer/` (`mplayer.conf`'s keys, the index's columns: appended only), the tray
  messages (appended only), `panel/items`' new kind `tray` and its new default.
- **Not frozen yet:** the GUI API (`windows.md` §6) and the kernel classes it adds
  (`Decoder::probe()`, `MidiDecoder::takeEvents()`). No SDK changes.

## 9. Steps

1. **Pure parts, host-tested** (done, 2026-10-09): `Playlist` (orders, shuffle, repeat, edits),
   M3U reading and writing, `Fft` and the bands, `MidiChannels`, the library index's lines.
2. **Toolkit** (done, 2026-10-09): `FolderItemModel` and `FileIcons` moved into `bmds_gui` (the
   file browser on them, unchanged), `ComboBox`, `TabWidget`, `Slider`'s release, `FileDialog`;
   each tried in `/bin/widgets` on the board.
3. **The player** (built 2026-10-09, after `rom.md`, which brought the `media-*` icons): the
   playback thread and the window (now playing, position, transport, volume, playlist), Open and
   Add, keys, errors, one player, `mplayer.desktop` and the file types. Checked by ear by the
   user: WAV, MP3, Ogg, FLAC and MIDI files, seeking, gapless.
4. **Kept between runs** (built 2026-10-09): `mplayer.conf`, `playlist.m3u8`, Open and Save
   Playlist.
5. **The tray** (§6e; built 2026-10-09, §11): `ServiceQueue`'s larger messages, the window
   manager's `watchServices()`, `TrayIcon`, the panel's `tray` area, the panel's volume control
   moved onto `TrayIcon` (the user checks it does all it did), then the player's icon and hiding
   on close.
6. **The visualizer:** measured on the board, then the default chosen.
7. **The MIDI view:** `takeEvents()`, the model, the view, the font's choice.
8. **The library:** `Decoder::probe()` (with the fuzzers), the scanner, the index, the view,
   the watches.
9. **As built** (a new §11), `roadmap.md` and `audio.md` updated.
10. **Tray messages** (`TrayIcon::showMessage()`): the balloon, after the player is done. Done
    2026-10-10, brought forward as the system's notifications (`notifications.md`): a tray
    message is a notification; the player says "Now playing" while hidden, with Previous, Pause
    and Next.

## 10. Decided (the user, 2026-10-09)

- **The MIDI view's font choice is the player's own,** saved in `mplayer.conf`, `synth/font`
  untouched; "Use as the system's default" beside it sets `synth/font` too.
- **The tray icon shows while the player runs,** on a proper system tray API (§6e). The volume
  control uses the same API but stays in the panel's process (no program of its own).

## 11. As built

- **Step 1, the pure parts** (2026-10-09), in `components/bmds_mplayer/src` (the component has no
  program yet) and `bmds_core`'s `Fft`; host-tested (`tests/host/mplayer.cpp`, 11 cases):
  - **`Playlist`:** in order to the end, Repeat All and One (Next still moves on), Previous
    stopping at the first; inserts, removals and moves keeping the current entry; removing the
    one playing leaves none current and its follower next (also when one is inserted in its
    place, or it was the last); 2,000 changes of information compacting the texts; `Full` at
    10,000. Shuffle, over 50 seeds: the chosen entry first, each of 20 once a round, the next
    round (Repeat All) never starting with the last one; one added mid-round plays in it, one
    removed doesn't; turning it off keeps the entry; moves keep it.
  - **M3U:** a byte order mark, CR LF, comments, `#EXTINF` with attributes (a comma in quotes),
    `-1` lengths, backslashes, `..`, absolute paths, a `file://` URL with `%20`, an `http://` one
    skipped; written relative under the list's folder and read back the same; a list at `/`.
  - **`Fft`:** every size from 4 to 2,048 points against a plain DFT in double (within 1e-5·N);
    a full-scale sine's peak N/2; sizes not a power of two, too small or too large refused.
  - **`Spectrum`:** full-scale sines at 250 Hz, 1 kHz and 5 kHz loudest in their own band (within
    a band of where they should be), between −3.5 and +0.5 dB, the 16 kHz band below −40 dB;
    silence at the floor. **`MeterBars`:** up at once, 20 dB a second down, the mark held a
    second.
  - **`MidiChannels`:** programs, volume, notes (a note-on of velocity 0 as an off) applied only
    when the played frames reach them; the loudest sounding, strikes taken once; beats at 120
    and 60 a minute, bars across a change to 3/4; a reset; the queue's overflow noted.
  - **`LibraryIndex`:** sorted by path, replaced by path, tabs, line breaks and backslashes
    escaped and read back, every field round-tripped, bad lines counted (too few fields, a word
    for a number, no path, a year too large), extra fields ignored.
  - `MidiDecoder::Activity` (the events a MIDI file's playing will report, step 7) is declared in
    `mididecoder.h` so the model and the decoder share it.
- **Step 2, the toolkit** (2026-10-09), in `bmds_gui` for every program:
  - **Moved:** `FolderItems` became `FolderItemModel` (the clipboard's cut files now optional,
    `setCopied()`), with `FileIcons`, out of the panel; its folder windows use them unchanged.
  - **`FolderModel::setNameFilter()`** and **`NameFilter`** (`bmds_files`, pure): `;`-separated
    `*` and `?` patterns without ASCII case, files only (folders always shown); `NoFiles` for
    choosing a folder; `firstExtension()` for Save's default extension.
  - **`ComboBox`:** a field with an arrow; a press, Alt+Down, Alt+Up or F4 opens a `Menu` of the
    items under it (made at the first opening, the current one checked, `&` doubled); Up, Down,
    Home, End and the wheel step. Items carry a value (`itemData()`).
  - **`TabWidget`** (its row's geometry and stepping pure, `TabRow`): pages are its children, only
    the current one visible; the focus moves to the new page's first focusable widget if it was
    in the old one. Its keys come through a new **`Widget::keyBeforeChild()`**: the focus
    widget's ancestors are offered a key first, innermost first, as a list would otherwise take
    Ctrl+PageDown for itself.
  - **`Slider`:** `Released` (after a drag, or a press on the groove: the press marks it dragging
    before moving the handle there) and `isDragging()`; keys and the wheel only `Changed`.
  - **`FileDialog`** (§6d, a `Dialog`): Open, OpenMany, Save, Folder. Its own `FileIcons`; the
    folder read 64 entries a turn; a directory watch while it is open; the view reset as soon as
    a folder opens (else its old current row's index named a row of the new folder). The name
    field: one name, or several in quotes (`FileNameList`, pure); a folder typed is entered, a
    pattern filters. Escape in a field, the close box and Cancel reject. Also new:
    `GuiApplication::cancelPosts()` (the dialog's destructor drops its own posted calls) and
    `Label::setMinimumWidth()` (its labels in one column).
  - **Host tests:** `NameFilter` and the filtered `FolderModel` (`folders.cpp`), `TabRow` and
    `FileNameList` (`gui.cpp`); 449 cases pass.
  - **On the board** (`/bin/widgets`, driven with `win pointer` and `win hid`, seen in
    screenshots): Files and Controls tabs by click, Ctrl+Tab and Ctrl+PageDown (from the tree
    too); the combo box's menu (an item with `&` shown as itself) and its choice told; the seek
    bar showing "(dragging)" then told on release. The dialog: places (Home, `/boot`, `/ram0/p0`,
    `/sd0/p0`, `/`), the media filter showing `collectathon.wav` among `/home`'s files, Enter on a
    folder entering it, Backspace going up with the folder left selected, Open choosing the file;
    Save adding `.m3u8` to `playlist`, and asking before replacing `collectathon.wav` (No kept it
    open); Choose Folder; Open Several with Ctrl+A in `/boot` taking the four files, not the
    folders; `p*` typed leaving `profile` and the folders. Ten openings (`mem mark`, `mem new`): 5
    blocks (496 bytes) new, among them the shell's prompt and the last folder's path; nothing
    tenfold. The file browser's windows as
    before.
  - **The user's look** (2026-10-09) found three things, fixed and checked on the board:
    - the current tab was drawn first, so the next tab's lit edge drew over its right edge
      (seen only with the first tab current): the current tab is drawn last now;
    - `DetailsView`'s header lies outside the viewport a scroll redraws, so scrolling across
      left it behind (plainly in an empty folder): `scrolled()` redraws it when the scroll
      across changes;
    - several files: Ctrl+click and Shift+click select several in Open Several (checked with
      `win hid` modifiers: four by Shift, two by Ctrl, folders left out of the name field). The
      names a selection put in the field are cleared on going to another folder (where they name
      nothing); a name edited since is kept.
- **Step 3, the player** (2026-10-09), `components/bmds_mplayer`: `/bin/mplayer`
  (`BMDS_PROGRAM_ICONS`, flat-color's `music` built in), 29 KB of flash with step 1's parts, no
  static RAM.
  - **`Player`** (`player.cpp`), the playback thread (8 KB PSRAM stack, the shell's priority + 1):
    commands in a queue of 8 under a lock, woken by a semaphore; a command that throws the queue
    away (a new track, a seek, Stop) also drops the stream from the loop, which wakes a write
    waiting for room or a drain at once. Writes of 20 ms. `/dev/dsp` is opened at the first track
    and closed at a stop or the list's end (its ring, 500 ms asked, 743 ms at 44.1 kHz, freed).
    A track is a number from the loop; `play()` with the open track's number seeks in it. Its
    reports (a queue of 16, the loop told through `post()`): Started (the track's stream frame and
    file frame, its information and tags), Ended (all decoded: the loop answers with the next,
    `Follow`, or `finish()`), Failed, Stopped. Pause is the stream's own and the thread's (no
    writes while paused; a seek while paused stays paused).
  - **Gapless:** the next track goes into the same stream when the rate and channels match (the
    stream's frame counter goes on); else the thread drains, configures again (counting from 0),
    and the report says the marks before are over.
  - **Where it is:** `TrackClock` (pure, host-tested): marks of (track, stream frame, file
    frame); the frames heard are the stream's played frames less what the device still holds
    (`delayFrames` − `queuedFrames`), moved on by the time since the status (at most 50 ms) while
    playing. The window shows the next track when the frames heard reach its mark, so the title
    changes when it is heard, not when it is decoded. A seek shows its target until it starts.
    Redrawn 4 times a second only while playing.
  - **The window** (`playerwindow.cpp`): the title, artist and album, the format line (`MP3 ·
    44.1 kHz · stereo · 192 kbps · 0:30`), the time played and left (`~` for an estimate), the
    seek bar (seeks on release, or at once by keys and the wheel), the transport (icon buttons with
    tooltips: previous, play or pause, stop, next, repeat cycling Off, All, One with its icon,
    shuffle), mute (its icon by the level, as the tray's) and the volume (−60 to 0 dB, the wheel
    1 dB a notch, its dB in the tooltip), the playlist in a tab (Title with the playing or failed
    entry's icon, Artist, Album, Length, Format, filled in as each file starts), a status bar
    (the state or a message for 5 s; the entry's number, the count and the total time, `+` for
    unknown lengths). Menus File (Open, Add Files, Add Folder, Close, Quit), Playback (with
    Repeat as a submenu, Shuffle checked), Help.
  - **Keys:** Space, Left and Right (5 s), Ctrl+Left and Ctrl+Right, Ctrl+Up and Ctrl+Down (move
    the entry), Delete, Return (play the row), Ctrl+O, Ctrl+W and Ctrl+Q; in the list too (its
    view offers them to the player first).
  - **Adding:** files, M3U playlists (read as step 1's `M3u`), folders (their sound files sorted
    without case, then their folders, 6 deep). Previous restarts a track past 3 s. Next and
    Previous in a gapless change's last moments go to the track already queued.
  - **Errors** in the status bar, the entry marked with `dialog-error` and its reason as the
    row's tooltip: "Not a sound file this player knows", "damaged at 1:23", "No SoundFont"
    (a MIDI file whose font is missing), the device in exclusive use (it stops). A failed entry is
    passed over even with Repeat One; every entry failing in a row stops it.
  - **One player:** the service `mplayer`; a second `mplayer [-a] FILE...` sends the files
    (absolute) to it and exits (0, or 1 with the reason): a message is a header (`uint32_t size`,
    a request: 1 Show, 2 Open, 3 Add) and paths each ending in a NUL, the first message Open and
    the rest Add when they need several. `mplayer` alone shows the window.
  - **File types:** `.m3u` and `.m3u8` are `audio/x-mpegurl` (text-like); `mplayer.desktop`
    (`Exec=mplayer %F`); `/boot/config/mimeapps.list`'s default gives `audio/*` to it before
    `play.desktop` (a `/boot` made before keeps its own file: the manifest installs only new ones).
  - **Not yet, as planned:** closing while playing quits (hiding comes with the tray, step 5); the
    MIDI voices in the status bar come with the MIDI view (step 7); a MIDI file's own name (its
    first track's) isn't a tag the decoder gives (step 8's `probe()` reads it). No `#` column: the
    view's first column is the name, so the entry's number is in the status bar.
  - **On the board:** MP3 into Ogg into FLAC into WAV without a stop, seeking by keys while
    playing and paused, pause and resume, Next, Stop (the device stops), the list's end (stopped,
    the device closed), a PNG passed over with its reason, `mplayer -a` and a missing file from a
    second instance, a folder (`mplayer /home`), a MIDI file through the synth (the font loaded
    in 739 ms), the volume by the wheel (the stream's gain), moving and deleting entries (the one
    playing deleted plays on), the Open dialog. Two sessions under `mem mark`: nothing left after
    the second (the first left 19 KB of internal blocks, by their sizes the audio device's period
    buffers, made at its first start since boot).
  - **Host tests:** `TrackClock` (seeks, gapless changes, marks past `MaxMarks`, a fresh stream;
    the frames heard from a status), `TimeText`, the M3U file type; 459 cases pass.
- **Step 4, kept between runs** (2026-10-09), in `/home/.config/mplayer/`:
  - **`mplayer.conf`** (`PlayerSettings`, pure, host-tested): `[Playback]` Volume (dB), Muted,
    Repeat (off, all, one), Shuffle, Entry (from 1; none: no key), Position (ms); `[Window]` Width
    and Height (the frame's included), Tab; `[Folders]` Last. Keys appended only; a value out of
    range or unreadable leaves its default. The visualizer, the library's folders and the MIDI
    font are appended in their steps.
  - **`playlist.m3u8`:** `M3u::write`, with the lengths and titles learned from the files.
  - **When:** 2 s after the last change (volume, mute, repeat, shuffle, an edit of the list, the
    entry playing, the dialogs' folder), and at quitting while the thread still runs, so the
    position is the one heard. A title or length learned from a file doesn't start the timer:
    it goes with the next write (a long list isn't written at every track).
  - **Written safely:** to `NAME.new`, the old one removed, then renamed (`Vfs::rename` doesn't
    replace); reading falls back to `NAME.new` if `NAME` is missing (a power cut between the two).
  - **At start:** the size before the window is made (the default below 320 x 240); after
    `build()` the volume, repeat, the list, shuffle (a new order), the entry selected with its
    name and position shown; nothing plays until asked, and Play starts from the position, once
    (Stop, or an edit that moves entries, forgets it). `mplayer FILE...` still replaces the list.
  - **The size** is kept from `resized()`: the window's rect is gone when the manager closes it.
  - **File:** Open Playlist... (a playlist replaces the list and plays) and Save Playlist As...
    (`FileDialog` Save, `Playlist.m3u8` offered; paths under its folder written relative). The
    folder of the last dialog accepted opens all of them next time.
  - **On the board:** the files written 2 s after a change and at closing; a second start put
    back the list, the entry and 1:18 (Play went on from there), repeat and shuffle; Save As to
    `/home` (paths `Music/...`), then Open Playlist of it (relative paths read, played shuffled);
    `Last=/home` kept; no `.new` left; a session under `mem mark` left nothing. The component is
    now 34 KB of flash (5 KB more), no static RAM.
  - **Host tests:** `PlayerSettings` (defaults, reading, bad values, written and read back); 460
    cases pass.
- **Step 5, the system tray** (2026-10-09):
  - **Window manager:** `ServiceQueue` takes 2 KB messages, 8 KB waiting; `watchServices()`
    (told on any service named or gone, the manager locked: it wakes a loop), `hasService()`,
    `unregisterService()`; a window's `closedAt` (told where it was as it closed, instead of
    `closed`), so `GuiWindow::lastRect()` can open it there again.
  - **Toolkit:** `GuiApplication` has 4 services, `unregisterService()`, `watchServices()` (4
    watches, one notification for any number of changes) and `hasService()`. `TrayProtocol`
    (the messages: `Header`, `Update`, `Pixels`, `Event`, each `size` first, read with the
    fields a sender didn't know zero), `TrayRow` (pure: the icons by reply service and number,
    ordered by category then arrival, 16 at most, a 24-pixel cell each) and `TrayIcon` (Qt's
    shape: theme icon or 16 x 16 pixels, tooltip sent only when it changes, category, context
    menu opened at the icon, `Activated` with Trigger, DoubleClick, MiddleClick or Context,
    `Scrolled`, `geometry()`, availability). An application's icons share one reply service,
    `tray.<connection>` (the plan said the pid; the connection is as unique and at hand), and
    one service watch; they are sent again when `tray` comes back.
  - **Panel:** `TrayService` (the service `tray`, kept while the panel runs so the icons outlive
    its items being made again; started while `panel/items` has a tray) and `TrayArea` (the
    item: cells, a thin edge under the pointer, sunken while pressed, tooltips; a press tells the
    program, a left one twice within the double-click time also DoubleClick; the wheel;
    `Geometry` sent to each program after the row moves). `panel/items`' default is
    `launcher;windows;tray;clock`; a saved `volume` is read as `tray` where the list has none.
    `VolumeItem` became `VolumeControl`, a `TrayIcon` (Hardware): click the slider, wheel 1 dB,
    middle button mute, right click Mute and Mixer... (disabled until the mixer window). The cell
    isn't sunken while the slider is open any more (the tray doesn't know of the popup).
  - **Player:** its icon (`multimedia-player`) while it runs; the tooltip "Title — Artist · 0:46
    / 2:56" (" (paused)"), or "Media Player: stopped"; a left click shows the window where it
    was and raises it; a right click Play/Pause, Stop, Previous, Next, Show Player, Quit. Close
    (the frame's, File > Close, Ctrl+W) while playing or paused, with a tray there, closes the
    window and plays on; stopped, or with no tray, it quits. `mplayer` with one running shows it.
  - **On the board:** the volume in the tray (slider by it, wheel 0 to -3 dB and back, Mute from
    its menu with the muted icon, the middle button unmuting); the player's icon left of it, its
    tooltip following the time, closed while playing (sound on), Next from its menu, shown again
    by a click where it was moved to, Quit from its menu (the icon gone); the panel killed and
    started again with the player running: both icons back. Two sessions under `mem mark`: the
    second left nothing (the first, 0.6 KB of lists grown once).
  - **Cost:** 6.2 KB of flash for the tray (`TrayIcon` 2.1 KB, `TrayRow` 1.5 KB, `TrayArea` and
    `TrayService` 2.7 KB), more than §7's 3 KB; the player 1.1 KB more. No static RAM. Per
    program with icons: a 2 KB service buffer (PSRAM); the tray: about 1.4 KB an icon.
  - **Host tests:** `TrayProtocol` (older shorter messages, lying sizes, unended strings, cutting
    on a character) and `TrayRow` (order, hiding, updates, pixels, removal, programs gone,
    `Full`); 462 cases pass.
  - **After the user's look** (2026-10-09):
    - **Tooltips were unsteady:** every tooltip change (the player's, each second) went the way
      of a row that moved: the hover forgotten, the places sent again. `TrayRow::update()` now
      says whether the icons moved; a change of look only redraws, and re-shows the tooltip if
      it is that icon's.
    - **One notification area:** `TrayArea` and the clock's label became `NotifyArea`, one
      widget drawing the icons and the clock in one sunken field (`panel/items`' `tray` and
      `clock` side by side, in their order; either alone is an area of its own). The clock has a
      character's room each side (two before). Found on the way: text and icons must be drawn
      from a column (`GuiStyle::Column`), so a 1-pixel bevel's inside needs aligning, which is
      why the first try (a box around the two) drew nothing.
    - **Flat icons:** no edge under the pointer or while pressed.
    - **The clock:** `panel/clock/format` defaults to `%l:%M %p` (" 1:00 AM", the hour padded
      with a space); the new `panel/clock/tooltip` (default `%A, %B %-d, %Y`, "Thursday, January
      1, 2026") is its tooltip, made again each tick so it turns at midnight. `formatTime()`
      gained GNU's flags (`%-d` not padded, `%_H` spaces, `%0e` zeros). Between the icons and
      the clock, 8 pixels (12 before): text and icons start on a 4-pixel column, so half of 12
      can't be.
    - **Narrower cells:** 20 pixels (24 before), 4 between icons, a column's room before the
      first; the clock still 8 pixels after the last.
    - **Los Angeles:** the board's `clock/zone` is `America/Los_Angeles` (saved). After a
      reboot, unset, it read 06:15 PDT (13:15 UTC) 11 s after boot: the name resolved from the
      card's zone database and the start was worked out then, the local reading unchanged.
    - **Opening from the file browser:** `mplayer -p FILE...` (and the service's request 4,
      Play): a sound file already in the list is played there, one that isn't is added at its
      end, folders and playlists are added; the first plays from its start, the window shown.
      `mplayer.desktop` runs `mplayer -p %F`. Plain `mplayer FILE` keeps replacing the list (a
      shipped interface).
    - **Tab and Shift+Tab** in the player move the focus through the seek bar, the buttons, the
      volume and the list, and around, on the board (`win hid`); not changed until the case that
      failed is known.
