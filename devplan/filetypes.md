# File Types and Opening Files

Status: **built** (as-built: §10) (2026-10-07, the user's request: "I expect we'll want to support regular
files on the desktop too, where 1. we've got an SDK API that'll tell us what format the file is
(probably be a combination of looking at the extension and in the case we don't know,
inspecting the file contents). This function will need to be fast. 2. we've got a configuration
that defines how to open files (e.g. we'll use vi in a terminal for text files, etc).")

## 1. Goal

- **Tell what a file is**, by an SDK call that every program can make. It uses the name's
  extension, and when that says nothing, the file's first bytes. It is fast: by the name alone,
  it reads nothing.
- **Open a file with its application**, from a configuration that maps types to applications:
  text in vi in a terminal, and so on.
- **Show every file on the desktop**, not only Desktop Entry files. Each has its type's icon,
  and a double click opens it.

## 2. Facts that shape the design (checked 2026-10-07)

- **Types are named as MIME types** already: the clipboard's (`clipboard.md`), and freedesktop.org's
  Desktop Entry files, whose `MimeType=` key lists types.
- **freedesktop.org's `mimeapps.list`** maps a type to Desktop Entry files:
  `[Default Applications]`, `TYPE=ID.desktop;...`. The launcher's entries are already Desktop
  Entry files, in `/boot/applications` and the card's `share/applications` (`gui.md` §4c).
  BusyBox's `vi.desktop` is one (Terminal=true).
- **freedesktop.org's icon names for types:** the type with `/` made `-` (`text-plain`), then a
  generic one (`text-x-generic`, `image-x-generic`, `audio-x-generic`, `video-x-generic`,
  `package-x-generic`, `application-x-executable`), then `unknown`. Flat Color has pictures
  for them under other names (`document`, `image_file`, `audio_file`, `video_file`, `package`,
  `file`, `folder`); the theme's aliases file links names to pictures (`dist/icons/aliases`).
- **The desktop's entries** are read by `readEntryDirectory()` (`.desktop` files only) into
  `PanelEntry`s, at most 32. A `Desktop Entry`'s `Exec` field codes (`%f`, `%u`...) are
  dropped by `splitExec()`.
- **The system call table** is append-only (`docs/ABI.md`); the last slot is 57
  (`thread_clear`).
- **FAT has no execute permission;** programs are found by their ELF header (`program.h`).

## 3. Decisions

| # | Question | Choice | Alternatives |
|---|---|---|---|
| T1 | How a type is named | **A MIME type, freedesktop.org's name for it** (`text/plain`, `image/png`, `application/x-executable`, `inode/directory`). The same names as the clipboard's | our own names |
| T2 | How a type is found | **The name first:** a table of extensions (case-insensitive) and whole names (`Makefile`). Found: done, with nothing read. Not found: the first 512 bytes, checked against magic numbers (ELF, PNG, JPEG, GIF, PDF, ZIP, gzip, xz, bzip2, tar, RIFF WAVE and AVI, Ogg, FLAC, MP3's ID3, `#!`, `[Desktop Entry]`, `<?xml`, HTML), then for text (valid UTF-8, no NUL, only the usual controls): `text/plain`, else `application/octet-stream`. An empty file is `application/x-zerosize`. A directory is `inode/directory`, a device `inode/chardevice` | the contents first (a read for every file: slow on the card) |
| T3 | Where the table lives | **`FileFormat` in `bmds_core`, pure, host-tested** (not `FileType`: `vfs.h` has an enum of that name): `byName()`, `byContent()`, `isText()`, `iconName()`, `genericIcon()`. Its strings are in rodata; the answers point into them | a file on the card (I/O for every lookup) |
| T4 | The SDK call | **A system call, slot 58: `int32_t file_type(const char *path, uint32_t flags, char *out, uint32_t size)`**. It writes the type, NUL-ended, and returns its length, or `-BMDS_ERANGE` (too small), `-BMDS_ENOENT`... `BMDS_FILE_TYPE_NAME` uses the name only (no I/O: a file not there still has a type); `BMDS_FILE_TYPE_CONTENT` ignores the name. `BMDS_FILE_TYPE_MAX` (127) is the longest type. In the OS, `Vfs::identify()` (bmds_fs, flags `IdentifyName` and `IdentifyContent`) | a library function in the SDK (each program carries the table, and a second copy of the code in C) |
| T5 | Text-like types | **Each table entry says whether it is text** (`application/json`, `application/x-shellscript`, `application/x-desktop`, the empty file...), as freedesktop.org's subclasses of `text/plain` would. `isText()` | a subclass database |
| T6 | The configuration | **`/boot/config/mimeapps.list`, freedesktop.org's format:** `[Default Applications]`, `TYPE=ID.desktop;ID2.desktop`. An ID is a file in the launcher's directories (`[Launcher] Applications`); the first found wins. A BMDS addition: `MEDIA/*` (`text/*`) matches a whole media type. Lookup order: the type, `MEDIA/*`, `text/plain` for a text-like type, then `application/octet-stream` | our own `open.conf` (the same thing in another format); `MimeType=` in entries (a later fallback) |
| T7 | How it is run | **The entry's `Exec`, its field codes filled:** `%f`, `%F`, `%u` and `%U` become the file's path (a local path for `%u`, which the spec allows a program to be given); an `Exec` without one gets the path at its end. `Terminal=true` runs it in `terminal -e`, as the launcher does. `DesktopEntry::splitExec()` gains the file (pure, host-tested) | — |
| T8 | Programs | **`application/x-executable` opens with `run.desktop`** (shipped, `NoDisplay=true`, `Exec=%f`, `Terminal=true`): a program runs in a terminal, so a command-line program's output is seen. A shell script is text: it opens in vi | running programs bare (a command-line program's output lost) |
| T9 | No application | **A message box:** "There's no application set to open TYPE files." | nothing |
| T10 | The desktop | **Every file of its directories but hidden ones** (a name starting with `.`). A `.desktop` file is a launcher as now (named without `.desktop`). Any other file is named by its whole name, and gets its type's icon (`iconName()`, then `genericIcon()`, then `unknown`). It opens with T6. Rename keeps its extension selected out: only the part before the last `.` is selected. 32 items at most, as now | only some types |
| T11 | Where the opener lives | **In the panel** (`panelapp.cpp`), the only program that opens files for now; the pure parts in `bmds_core` (`MimeApps`, `DesktopEntry`'s `Exec` filling). It moves into a library when a file manager or an `open` command needs it | a library now |

## 4. Design

### 4a. `FileFormat` (bmds_core, pure)

- **`StringView byName(StringView name)`:** the extension's type (from the last `.` that isn't
  the first character), or a whole name's (`Makefile`, `CMakeLists.txt`); empty if not known.
  A linear walk of about 70 short rodata entries, case-insensitive: microseconds.
- **`StringView byContent(ByteSpan head, bool whole)`:** the magic numbers, then the text check.
  `head` is the file's first bytes (512 asked for); `whole` says it is the whole file (an
  empty one is `application/x-zerosize`). A UTF-8 character cut at the buffer's end is not a
  fault unless `whole`.
- **`bool isText(StringView type)`:** `text/*`, and the table's text-like types.
- **`iconName(type, out)`:** `text/plain` → `text-plain`. **`genericIcon(type)`:** by its media
  (`text-x-generic`, `image-x-generic`, `audio-x-generic`, `video-x-generic`), archives
  `package-x-generic`, programs `application-x-executable`, directories `folder`, else
  `unknown`.

### 4b. `Vfs::identify()` (bmds_fs) and the system call

- `Result<StringView> Vfs::identify(StringView path, uint32_t flags, const Context &)`:
  - by the name, unless `IdentifyContent`;
  - `stat()` (links followed): a directory or a device;
  - else the first 512 bytes, read into the caller's stack;
  - the answer points into `FileFormat`'s table.
- **The system call** (`bmds_loader`): copies the answer out.
- **ABI:** the slot, the flags and `BMDS_FILE_TYPE_MAX` are appended to `sys.h`; `abilock.c`
  pins them.

### 4c. `MimeApps` (bmds_core, pure)

- `MimeApps::lookup(StringView text, StringView type, Span<StringView> out)` gives the IDs
  for a type: the type's line, then `MEDIA/*`'s. The caller tries them in order, then
  `text/plain`'s for a text-like type, then `application/octet-stream`'s.
- `[Default Applications]` only. `[Added Associations]` and `[Removed Associations]` are
  ignored.

### 4d. `DesktopEntry` and `Exec`

`splitExec(exec, buffer, bytes, args, file)`: with a file, `%f %F %u %U` become it, as one
argument (a field code inside an argument is replaced in place); without a field code, the
file is the last argument. `%%` is `%`; `%i %c %k` are dropped as now.

### 4e. The desktop

- **Reading:** `readEntryDirectory()` gets `allFiles` (the desktop's), which reads every file:
  - `.desktop`: a Desktop Entry, as now;
  - other files: an entry without `Exec`, and the type from `Vfs::identify()`.
- **Kept per item:** `PanelEntry` gets `type[48]`: empty for a launcher.
- **The opener** (in `panelapp.cpp`): the type, the IDs, the first entry found in the launcher's
  directories, its `Exec` filled, then started as the launcher starts entries. Where
  `mimeapps.list` is: `[Desktop] MimeApps` in `panel.conf` (default
  `/boot/config/mimeapps.list`).
- **The shipped defaults** (`boot/config/mimeapps.list`):
  - `text/*` and the text-like types: `vi.desktop`;
  - `application/x-executable`: `run.desktop` (`boot/applications/run.desktop`).
- **The theme** (`dist/icons/aliases`) gets the generic names:
  - `text-x-generic` document;
  - `image-x-generic` image_file;
  - `audio-x-generic` audio_file;
  - `video-x-generic` video_file;
  - `package-x-generic` package;
  - `unknown` file;
  - `folder` folder.

  The message box's `dialog-warning` (high_priority), `dialog-error` (cancel) and
  `dialog-question` (questions) come with them.

## 5. Budget (estimates)

- **Firmware:** the table and the magic numbers 3 KB; the opener and the desktop's reading 3 KB;
  the system call under 1 KB.
- **RAM:** none kept. 512 bytes of stack while a file's contents are read. `PanelEntry` grows
  48 bytes (32 desktop items: 1.5 KB of PSRAM).
- **Time:**
  - by the name, a table walk;
  - by the contents, an open and a 512-byte read: about a millisecond on `/boot`, a few on the
    card.
  - The desktop asks once per file when it loads.

## 6. ABI impact

- **Appended:** system call slot 58 `file_type`, `BMDS_FILE_TYPE_NAME`, `BMDS_FILE_TYPE_CONTENT`,
  `BMDS_FILE_TYPE_MAX`. A type's name, once answered for a file, doesn't change for the same
  file.
- **Frozen once shipped:** `/boot/config/mimeapps.list`'s format (freedesktop.org's) and the
  `MEDIA/*` addition.

## 7. Testing

- **Host:**
  - `FileFormat`: extensions, case, whole names, dotfiles, every magic number, text and binary,
    a UTF-8 character cut at the end, an empty file.
  - `MimeApps`: a type's line, the wildcard, several IDs, comments, other groups.
  - `splitExec()` with a file: each field code, none, inside an argument.
  - `Vfs::identify()` on the RAM filesystem: a directory, by name, by contents, the flags.
- **Board:**
  - An SDK program calls `file_type()` (`sdk/tests/progtest`).
  - A text file and a program put in `/boot/desktop`: each shows with its icon. A double click
    opens vi in a terminal on the text, and runs the program in a terminal. A file of a type
    with no application brings the message box.

## 8. Steps

1. `FileFormat`, `MimeApps`, `splitExec()` with a file (pure, host tests).
2. `Vfs::identify()`, the system call, the ABI lock, the SDK test.
3. The desktop's files, their icons, the opener, `run.desktop`, `mimeapps.list`, the aliases.
4. On the board.

## 9. Open questions

- **`MimeType=` in entries:** the fallback is built (2026-10-14, `apps.md` §13): for the type, then
  `text/plain` for a text-like one, an application whose `MimeType=` has it (or its `MEDIA/*`)
  opens it when `mimeapps.list` names none, before `application/octet-stream`. Still open: an
  "Open With" submenu.
- **User additions to the type table** (an extension file in `/boot/config`), read once at boot.
- **A file manager** and an `open` command, then the opener as a library.

## 10. As built (2026-10-07)

- **Pure** (host-tested in `tests/host/filetypes.cpp`):
  - `FileFormat`: about 60 names; every magic number; text, controls, Latin-1, a written-out
    U+FFFD, characters cut at the buffer's end (text) and at the file's end (binary); empty;
    text-like types; icon names.
  - `MimeApps`: other groups ignored, case, several IDs, the wildcard after the type's own
    line, a short output.
  - `splitExec()` with a file: each field code, one inside an argument, none (last), other
    codes dropped, too long.
- **`Vfs::identify()`** (host-tested on a RAM disk in `tests/host/files.cpp`):
  - The name wins unread.
  - A file without an extension is identified by its contents: a program, a script.
  - Also: an empty file; a directory; `/dev/null`; a name only, for a path that isn't there.
  - A character cut by the 512 bytes read still counts as text.
  - Reads end with `EndOfFile`, not a zero count.
- **Whole names first:** `CMakeLists.txt` is `text/x-cmake`, not any `.txt`.
- **The system call:**
  - `Process::fileType()` holds the path lock while it identifies, as `stat()` does.
  - Both flags together are `-BMDS_EINVAL`.
  - `abilock.c` pins the slot, the flags and the maximum; `BMDS_ABI_VERSION` stays 1 (not
    shipped yet, as for slots 53-57).
- **`progtest filetype PATH [name|content]`** (`sdk/tests/progtest.c`), run on the board with
  `tools/bmds-run`:
  - `mimeapps.list` is `text/plain` (`.list`);
  - `/boot/note.txt` by its contents is `text/plain`;
  - `autoexec.sh` is `application/x-shellscript`;
  - `random-seed` is `application/octet-stream`;
  - `/boot/config` is `inode/directory`; `/dev/null` is `inode/chardevice`;
  - `/nowhere/x.png` by its name is `image/png`; `/nowhere` gives `ENOENT`;
  - a 4-byte buffer gives `-ERANGE`.
- **The desktop:**
  - Files are named by their whole name, sorted with the launchers by name (a launcher by its
    `Name=`), and kept selected across reloads by their file's name.
  - The item's type is kept in `PanelEntry::type`.
- **On the board** (a test desktop on the card, the panel run with `-c`), the files showed with
  their icons:
  - a text file and a script as documents;
  - a PNG as a picture; a program (no extension) as a program;
  - random bytes and a PDF as a plain file; a directory as a folder;
  - a dotfile not shown.
- **Opening on the board:**
  - Typing n and Enter opened `notes.txt` in vi in a terminal (`terminal -e vi PATH`).
  - The PDF brought "There's no application set to open application/pdf files."
  - The program ran in a terminal through `run.desktop`.
  - F2 on `picture.png` selected only "picture"; typing "pic" and Enter gave `pic.png`.
  - Delete moved a file to the card's trash; Ctrl+C and Ctrl+V made `hello (copy)`.
- **The theme's new aliases** were linked on the card by hand (`ln -sf`), as
  `tools/install-icon-theme` will make them.
- **Not yet:**
  - Opening a directory: there is no file manager, so `inode/directory` has no application.
  - Launchers sort by `Name=`, not by the name shown.
