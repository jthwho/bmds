# Media Player

Planned 2026-10-09, not started; its main questions answered the same day (§4). The user's request
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

## 3. A shape to start from (to be agreed)

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

## 5. What the decisions bring with them (to be designed next)

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
