# Files: Copies on the Desktop, the Trash

Status: **built** (as-built: §10) (2026-10-07, the user's request: "add a paste to the desktop context menu
to paste a copied file. This should follow the new unique name for a copy if the file already
exists that's typical of this function. We should also add a delete (perhaps to trash
instead?). We probably need a trash paradigm in the OS (.trash/ folder at the root of the
storage device?)").

## 1. Goal

- **Paste a copied file** where the desktop's files are. A name already taken gets a copy's name,
  as file managers give it.
- **Delete to a trash** that every program shares: what is deleted can be restored to where it
  was, until the trash is emptied. The desktop's Delete uses it, and so does a command.

## 2. Facts that shape the design (checked 2026-10-07)

- **Copies are on the clipboard as `text/uri-list`** (`clipboard.md`): the panel's Copy offers an
  icon's `.desktop` file that way. `GuiClipboard::paste()` reads it on its own thread.
- **The desktop shows the Desktop Entry files** of its directories (`[Desktop] Icons`, default
  `/boot/desktop`), and nothing else (`gui.md` §4m).
- **Filesystems are mounted at paths** (`/boot`, `/sd0/p0`, `/tmp`: `Vfs::listMounts()`). A
  rename works within one, not across (`NotSupported`, which `mv` turns into a copy).
- **FAT keeps a modification time, no owners, no permissions;** every user is root. A name
  starting with `.` is hidden by `ls` without `-a`.
- **`/boot` is small** (4 MB) and slow to write (`storage.md` §13); the card is large.

## 3. Decisions

| # | Question | Choice | Alternatives |
|---|---|---|---|
| F1 | A copy's name | **`NAME (copy).EXT`, then `NAME (copy 2).EXT`, `(copy 3)`... the first free one** (GNOME's and KDE's form). The extension is from the last `.` that isn't the name's first character, so `terminal.desktop` gives `terminal (copy).desktop`, `.profile` gives `.profile (copy)`. A pure function, host-tested | Windows' `NAME - Copy.EXT`; macOS' `NAME copy.EXT` |
| F2 | Where the trash is | **On each filesystem, at its root: `MOUNT/.trash/`** (the user's choice of name). A file moves there by a rename, so deleting is as fast as renaming and works for directories. A file on `/boot` stays on `/boot`'s trash | one trash on the card (a copy across filesystems for every delete from `/boot`) |
| F3 | Its layout | **freedesktop.org's Trash specification inside it:** `files/NAME` (what was deleted) and `info/NAME.trashinfo` (`[Trash Info]`, `Path=` the original path, percent-encoded, `DeletionDate=` local time as `YYYY-MM-DDThh:mm:ss`). A name taken in `files/` gets ` (2)`, ` (3)`... The info file is written first, so a cut-off delete leaves an info file without its file, which is dropped, never a file that can't be restored | a single index file (one write for every delete; a cut-off write loses the index) |
| F4 | What can be trashed | **A file or directory on a mounted filesystem, other than the trash itself and a mount's root.** `/bin`, `/dev` and `/share/zoneinfo` aren't filesystems: `NotSupported` | — |
| F5 | The command | **`trash` (a shell command and `/bin` applet, frozen once shipped):** `trash PATH...` moves each to its filesystem's trash; `trash -l` lists the trash of every mounted filesystem (its name, the deletion date and the original path); `trash -r NAME...` restores them to their paths (an existing file is not replaced: an error); `trash -e [MOUNT]` empties every trash, or one mount's | `rm` sending to the trash (scripts expect `rm` to delete) |
| F6 | `rm` | **Unchanged:** it deletes. Only the desktop and `trash` use the trash, as on Linux | — |
| F7 | Paste on the desktop | **The desktop menu: Applications (the launcher's menu, as a submenu), a separator, Paste.** Paste is disabled unless the clipboard can be read as `text/uri-list`. Each `file://` path in the list is copied into the desktop's first directory, under a copy's name if its own is taken (F1). The desktop is then reloaded. A file that isn't a Desktop Entry is copied but not shown (the desktop shows only those, §2) | Paste in the launcher's menu (it would show on the panel's Apps button too) |
| F8 | Delete on the desktop | **The icon's menu: Open, Copy, Delete;** Delete moves the icon's file to its trash (F2), and the desktop reloads. No confirmation: it can be restored (`trash -r`). Emptying the trash and restoring from the desktop come with a Trash icon, later | deleting outright (no way back) |
| F10 | New on the desktop (the user's, 2026-10-07) | **The desktop menu: Applications, New ▸ (Folder, Text Document, Empty File), Paste.** The new folder or file goes in the desktop's first directory as "New Folder", "New Text Document.txt" or "New File", or the first free "NAME (2).EXT", "(3)"... (Windows' form for new items). It is put in the cell where the menu was opened if that cell is free, selected, and its name edited at once (a file's extension left out of the selection) | templates from a directory (freedesktop.org's `Templates`), later |
| F9 | The terminal's menu | **A right click on a terminal opens Copy (disabled without a selection), Paste (disabled when the clipboard has no text) and Select All**, with their keys shown. The terminal isn't on the toolkit, so the window manager draws it: `WindowManager::showMenu()` (§4c) | the toolkit in the terminal (a second event loop) |

> **Changed (2026-10-17, `filebrowser.md` §4k):** the trash moves to `MOUNT/.bmds/trash` (FB18),
> `Path=` becomes relative to the filesystem's root (FB19), and `/trash` shows every trash as one
> folder (FB20-FB23). `trash -l` lists names as `/trash` shows them, and `trash -r` takes those
> names (or `/trash/NAME`), so the `MOUNT/.trash/files/NAME` form is gone.

## 4. Design

### 4a. Copy names (`bmds_core`, pure)

`CopyName::make(StringView name, unsigned n, Span<char> out)`: `n` 1 is ` (copy)`, 2 and up are
` (copy N)`, put before the extension (F1). The caller tries `n` = 1, 2, ... until the path is
free, up to 999.

### 4b. The trash

- **`TrashInfo` (bmds_core, pure):**
  - `format(path, local time, out)` writes the `.trashinfo` text; `parse(text, path out)` reads
    it.
  - Paths are percent-encoded as URIs are (RFC 3986's unreserved characters and `/` as they
    are). `Uri::encodePath()` and `Uri::decode()` are shared with the panel's Copy.
- **`Trash` (bmds_fs):**
  - `move(path, context)`: the path's mount, its `.trash/files` and `.trash/info` made if
    missing, a free name, the info file written, then the rename. Returns the name used.
  - `list(fn, arg)`: every mounted filesystem's trash, each entry with its mount, name, original
    path and deletion date.
  - `restore(mount, name, context)`: back to its path, if that is free and its directory exists.
    Then the info file is removed.
  - `empty(mount or every one)`: the files removed (directories whole), then their info files.
- **`Vfs::copyFile(from, to, context)`** (bmds_fs): a file copied to a new path (`Create` and
  `Exclusive`), its modification time kept. The desktop's Paste uses it; a delete across
  filesystems never happens (F2).

### 4c. `WindowManager::showMenu()`

`showMenu(id, items, x, y, fn, arg)`:

- Items are labels, shortcuts and `MenuLayout` flags (`Disabled`, `Separator`), copied, up to
  `MaxMenuItems` (10). They are drawn and run like the window and desktop menus.
- The menu opens at (x, y) of window `id`'s client area, kept on the screen.
- `fn(arg, index)` is told the choice after the round, without the window manager's lock, as
  the desktop menu's is. Nothing is told when it's dismissed.
- It is the desktop menu's mechanism with its own labels; the menu is one popup at a time,
  whoever opened it.

### 4d. The terminal

- **A right press** asks the terminal's worker (the mouse callback must not call the window
  manager) to open the menu: Copy (`Ctrl+Shift+C`), Paste (`Ctrl+Shift+V`), a separator, Select
  All.
- **Select All** selects the history and the screen as lines (`TerminalSelection`), which
  becomes the primary selection as a drag's does.

## 5. Budget (estimates)

- **Firmware:**
  - copy names and the trash's format: 1 KB;
  - `Trash` and `copyFile`: 3 KB;
  - `trash`: 2 KB;
  - `showMenu`: 1 KB;
  - the terminal's menu and the panel's items: 2 KB.
- **RAM:** `showMenu`'s labels in the window manager's `.bss`, about 500 bytes. Nothing else
  while idle.
- **Flash writes:**
  - a delete writes an info file, two directory entries and the FAT;
  - a paste writes the copy;
  - on `/boot`, each is one or two of its 4 KB blocks.

## 6. ABI impact

Frozen when shipped:

- `trash`'s interface (F5);
- the trash's layout on disk (F2, F3: what other systems' tools could read);
- the copy names' form (F1).

The commit carries an `ABI:` line. `showMenu` is internal, as the window manager's API is
(`windows.md` §6).

## 7. Testing

- **Host:**
  - copy names (extensions, dotfiles, long names cut to fit);
  - the trash info format (encoding, parsing, a bad file);
  - `Trash` on RAM disks: move, a name taken twice, list, restore (and refused onto an existing
    file), empty with a directory, the trash's own path refused;
  - `trash` through the shell;
  - `copyFile`.
- **Board:**
  - the terminal's menu by `win pointer` (Copy, Paste, Select All, the disabled items);
  - the desktop's Copy then Paste (`terminal (copy).desktop`, then `(copy 2)`);
  - Delete, then `trash -l`, `trash -r` (the icon back), `trash -e`.

## 8. Steps

1. **Pure parts** (host-tested): `CopyName`, `TrashInfo`, `Uri`.
2. **`Vfs::copyFile`, `Trash`, `trash`** (host-tested on RAM disks).
3. **`showMenu`** and the terminal's menu.
4. **The panel:** the desktop menu with Paste, the icon menu's Delete.
5. **On the board.**

## 9. Open questions

- A Trash icon on the desktop (open it to restore, a menu to empty it), and a file manager.
- Showing files other than Desktop Entries on the desktop (F7 copies them unseen).
- The trash's size: nothing removes old entries by itself (freedesktop leaves it to the user).
- The desktop doesn't see changes made elsewhere (`trash`, `cp`, `rm` in a shell): it reloads
  after its own Paste and Delete, when the panel starts, and while icons are missing at boot.
  Being told of changes to its directories needs the VFS to say so (its change hook is one
  global caller now).

## 10. As built (2026-10-07)

- **Pure** (host-tested in `tests/host/files.cpp`): `CopyName::make`, `Uri::encodePath` and
  `decode`, `TrashInfo::format` and `parse` (`bmds_core/filenames.h`).
- **`Vfs::copyFile`:**
  - A new file, the modification time kept; a partial copy removed.
  - An existing target is `Exists` before anything is opened. On the board, the first name
    tried, the file itself, came back `Busy` from the write's open, which stopped the copy
    names' search.
- **`Trash`** (bmds_fs):
  - `move`, `list`, `restore`, `empty`, `mountOf`. Directories are emptied a name at a time,
    closed in between.
  - Host-tested on RAM disks:
    - a name taken twice gets ` (2)`;
    - a directory, named relative to the current directory;
    - refused: a mount's root, the trash and what's in it, a missing file, `/bin`;
    - an info file without its file isn't listed;
    - restoring onto an existing file is refused;
    - emptying one mount, then all.
- **`trash`** (`FileCommands::trash`), as F5. `-r NAME` finds the name in whichever trash has
  it; with several, it asks for `MOUNT/.trash/files/NAME`.
- **`WindowManager::showMenu()`:** the desktop menu's popup factored out (`openMenuLocked`). A
  program's choice is told after the round, unlocked, as the desktop's is.
- **The terminal:**
  - A right press asks its worker to open Copy (disabled without a selection), Paste
    (disabled unless the clipboard can be read as text) and Select All.
  - Select All takes every line from the history's first to the screen's last with text (or
    the cursor's), as lines, into the primary selection.
- **The panel:**
  - The desktop menu (Applications, Paste) and the icon menu's Delete, as F7 and F8.
  - Paste reads `text/uri-list` with `GuiClipboard::paste()`. From the panel's own copy, it is
    rendered on its loop with no pipe.
- **On the board:**
  - The terminal's menu, with nothing selected and an empty clipboard, showed Copy and Paste
    disabled. Select All put the terminal's 51 bytes in the primary selection; Copy put them on
    the clipboard; Paste typed a command that ran.
  - Copy on the Terminal icon, then Paste twice on the desktop, made
    `terminal (copy).desktop` and `terminal (copy 2).desktop`, three icons shown.
  - Delete on the third moved it to `/boot/.trash/files`, its info holding
    `Path=/boot/desktop/terminal%20%28copy%202%29.desktop`.
  - `trash -r` put it back; `trash` on both copies, `trash -l`, then `trash -e` emptied
    `/boot/.trash`.
  - The deletion dates read 2026-01-01: the board's clock wasn't set.
- **Size:** the image grew 12,192 bytes to 843,248, with the trash, copy names, `copyFile`,
  `trash`, `showMenu` and both menus.
- **New (F10, 2026-10-07):**
  - New ▸ Folder, Text Document and Empty File, each with its type's icon.
  - A submenu's choice is told to its root menu's function. So the desktop menu's own items
    now have negative numbers (Paste -2, the New items -3 to -5), and 0 and up are the launcher
    entries chosen from its Applications submenu. Before this, choosing the first application
    there was taken as Paste.
  - **On the board:** a right click on an empty cell, then N and T, made
    `New Text Document.txt` in that cell (`desktop.conf` 4,3), with the editor over its name
    and "New Text Document" selected. Typing "notes" and Enter gave `notes.txt`. N and F at
    another cell made `New Folder` there.
- **The trash through links (2026-10-07, found trying `filebrowser.md` step 2):** `/home` is a
  link the VFS provides, so `trash /home/Desktop/X` and the desktop's Delete failed ("not on a
  filesystem with a trash"): the path was matched to a mount without following links. Now its
  folder is resolved with `Vfs::realPath()` (its own name kept, so a link is trashed itself) and
  the physical path is what the info file records. Host-tested through a linked folder.
