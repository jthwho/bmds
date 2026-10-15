# Applications: one folder each

Status: **steps 1 to 4 built** (2026-10-14, §11 to §13); planned with the user 2026-10-14. Comes before the games (`games.md`), which are
installed this way.

## 1. Goal

An application is **one folder** on a card: its program, its data, its description. Installing
is copying the folder; removing is deleting it; updating is replacing it. The system finds
installed applications by looking, with no registry and no install step, and every one is a
command without touching `PATH`. The models are RISC OS's application directories (`!App`,
registered when the filer sees them) and the Mac's `.app` bundles; BMDS's is simpler than either.

Not goals: a package manager; dependencies between applications (programs are statically linked
against musl, so a folder is self-contained already); moving Unix ports out of the usual layout
(BusyBox, Dropbear and the like keep `bin/` and `share/`, `distribution.md`: they expect it).

## 2. Decisions (the user's, 2026-10-14)

- One folder per application, holding the program and whatever data it needs; installing is
  copying it over; the system detects every application installed.
- **Short names** (they are typed): the folders are in `apps/` at a card's root
  (`/sd0/p0/apps`), not `Applications/`.
- **Folders keep an extension**, `NAME.app`, which is how they are recognized.
- **A virtual `/apps`** lists every installed application's command, so one `PATH` entry covers
  them all (§5).
- **An application's own data is in the user's home**, not `/cfg`: `/cfg` is the system's
  configuration (§6).

## 3. The folder

```
/sd0/p0/apps/blocks.app/
        app.desktop             what it is: a Desktop Entry (below)
        blocks                  the program (any name; Exec names it)
        controls.conf           its actions and default bindings (optional, controls.md §5a)
        ...                     its data, laid out as it likes (sounds/, music/, levels...)
```

- **`app.desktop`** is a Desktop Entry in the format the launcher already reads (`gui.md` §4d,
  `DesktopEntry`): `Name`, `Comment`, `Exec` (a path relative to the folder: `Exec=blocks`),
  `Icon` (optional: the program's own icon notes are used without it, `gui.md` §4j),
  `Categories` (`Game`, `AudioVideo`...), `MimeType` (the types it opens, §7). BMDS's keys:
  - `X-BMDS-Id`: the application's id, which its data and the system's settings for it are kept
    under (§6). Default: the folder's name without `.app`. A written id survives the folder being
    copied under another name.
  - `X-BMDS-Command`: its command's name in `/apps` (§5). Default: the id.
  - `X-BMDS-Display`: the presentation it suggests, `Window` or `FullScreen` (`games.md` §3c;
    the user's setting overrides it).
- **The program finds its folder:** the loader sets `BMDS_APP_DIR` in a program's environment
  when the program it runs (links followed, so through `/apps` too) is in a `NAME.app` folder,
  whoever started it: the launcher, the file browser, the shell, `execvp`, and `BMDS_APP_ID` to
  its id (the user's choice, 2026-10-14: one source for the id, the system's). The SDK's
  `bmds_app_dir()` and `bmds_app_id()` return them (`bmds/app.h`). No system call.

## 4. Finding applications

- **Where:** a setting, `apps/path` (`;`-separated; paths are settings): default
  `/sd0/p0/apps`. More cards and partitions add their own roots; nothing assumes one.
- **How:** one service in the kernel (`Apps`) lists each root's `*.app` folders and reads each
  one's `app.desktop`: one small file per application (the time for a few dozen on the card's
  metadata cache is measured when built). No index file to fall out of date.
- **Kept current:** the VFS's directory watches on each root: a folder copied in, renamed or
  deleted is seen at once, and so is a card put in or taken out (mounts are watch events).
- **Who uses the list:** `/apps` (§5), the launcher (by `Categories`, as entries are now), Open
  With (§7). The launcher's search path of Desktop Entry files (`/cfg/applications`, then
  packages' `share/applications`) stays; folders are another source. Built-in programs (`/bin`,
  `/rom`) keep their entries in `/cfg/applications`. The same id from two sources: the first of
  `/cfg/applications`, folders, packages wins, so a user's entry can still override or hide
  (`Hidden=true`) anything.

## 5. `/apps`: every application a command

- **A virtual directory, as `/bin` is** (`programs.md` §17): read-only, an entry per installed
  application, each a **symbolic link to its program**:
  `/apps/blocks -> /sd0/p0/apps/blocks.app/blocks`. `ls -l /apps` shows what is installed and
  where. Every root is merged into it.
- **On the `PATH` once:** the default becomes `/bin:/rom/bin:/apps:/sd0/p0/bin`. Because the
  entries are real paths, everything finds them: the shell, `execvp`, BusyBox, `which`, tab
  completion, `#!`.
- **Names:** built-ins come first in `PATH`, so an application can't take one's name. Two
  applications with one command name: the first root's (in `apps/path` order) wins, and the
  other is logged.
- **`/apps` and `/sd0/p0/apps`** are different things with the same short name: the commands
  of all installed applications, and one card's folders. Accepted (the user's, 2026-10-14).

## 6. An application's own data

The folder is the application as shipped: it may be replaced at any time, so it is never
written. What an application writes goes in the user's home, under its id:

- **Its data** (saves, high scores, its own preferences): `$HOME/.local/share/ID/`, the
  freedesktop.org layout, on the card (Doom's saves are ~100 KB each). The SDK's
  `bmds_app_data_dir(buf, size)` makes it and returns its path.
- **What the system keeps for an application** (its presentation, `games.md` §3c) is the window
  manager's, in the user's home too (`$HOME/.config/bmds/apps.conf`, a group per id:
  `[blocks] Display=FullScreen`), and its bindings as the user changed them
  (`$HOME/.config/bmds/controls/ID.conf`, `controls.md` §5b). Not sysconf parameters: applications declaring their own would
  run into sysconf's 64 declared at most, and it isn't system configuration.

## 7. The desktop

- **The file browser** shows `NAME.app` as one item, with the application's icon and name; Open
  runs it, its menu adds Show Contents (opens it as a folder). Copying, moving and deleting it are
  a folder's.
- **Open With:** an application's `MimeType=` puts it in Open With for those types
  (`filetypes.md` T6's later fallback, now with a source); `mimeapps.list` still chooses the
  default.

## 8. ABI impact

Frozen once shipped (recorded with an `ABI:` line): the folder layout (`NAME.app`,
`app.desktop`), the `X-BMDS-*` keys, `BMDS_APP_DIR` and `BMDS_APP_ID`, `/apps` and its place
in the default `PATH`, the data directory's place. No system calls: `bmds/app.h`'s
`bmds_app_dir()`, `bmds_app_id()` and `bmds_app_data_dir()` are SDK library functions.

## 9. Steps

1. `Apps` (the scan, the watches, ids, overrides; host-tested on a fake tree), `apps/path`.
2. `/apps` in the VFS; the default `PATH`; `BMDS_APP_DIR` from the loader; the SDK's helpers.
3. The launcher from `Apps`; a test application (`hello` as a folder).
4. The file browser: one item, Show Contents; `MimeType=` into Open With.
5. Building folders: `bmds_add_app()` in the SDK's CMake, and `tools/bmds-dist` recipes
   installing into `apps/NAME.app`, so `programs/` and `dist/` produce folders.

## 10. Open questions

- Whether `/rom` should carry application folders too (built-in applications shipped this way);
  today's built-ins don't need it.
- Folders for more than one program (an application with helper programs): `Exec` names the
  main one; helpers are found through `BMDS_APP_DIR` and get no `/apps` entry.

## 11. As built: steps 1 and 2 (2026-10-14)

- **`DesktopEntry`** reads `X-BMDS-Id`, `X-BMDS-Command` (31 bytes at most each) and
  `X-BMDS-Display` (`Window`, `FullScreen`; anything else warned and ignored).
- **`Apps`** (`bmds_files`, portable, host-tested on RAM disks): each scan asks for the roots (a
  function: the system's reads `apps/path`), reads each root's `*.app` folders (the extension in
  any case) sorted by name without case, and each one's `app.desktop` (4 KB at most). Names (ids
  and commands): 1 to 31 of letters, digits, `.`, `_`, `+`, `-`, not beginning with `.` or `-`.
  The program is `Exec`'s first word, a relative path that stays in the folder (`bin/tour` is
  fine, `../x` isn't), and must exist. A folder that can't be used is kept as a problem with a
  reason (no `app.desktop`, unreadable or too large, not an application's entry, an invalid or
  taken id or command, `Exec` outside the folder, its program missing); `Hidden=true` hides it
  silently.
- **Kept current:** a `FolderWatches` on each root (a root not there: its parent instead) and on
  each folder found, so a folder copied in, an `app.desktop` edited or a folder removed is seen; a
  change of `apps/path` too (a `SysConf::Watcher`). Scans run on the service's thread (PSRAM
  stack, 12 KB: a scan on the boot task's 4 KB overflowed it), once changes have been quiet for
  300 ms (at most 2 s), so a folder being copied in is scanned once it is whole. A scan that
  changes nothing (a hash of what is installed and the problems) tells nobody. On the board the
  first scan (no `apps` folder) takes 1.5 ms of boot.
- **`/apps`** (`Vfs::setAppLinks()`): a link per command, resolved like `/home` (the link itself
  for `lstat()`, `readLink()` and changes; where it leads everywhere else); read-only, no mount
  there, listed in `/`, its watches told when the set changes. The `apps` boot stage (after
  `tmp`) starts the system's `Apps` with `/apps` following it.
- **`PATH`'s default** is `/bin:/rom/bin:/apps:/sd0/p0/bin` (the shell and the SSH service).
- **`BMDS_APP_DIR`:** the loader (a program started, and `execve`) follows the program's path's
  links (`/apps/NAME` included) and, when its folder is a `NAME.app`, sets `BMDS_APP_DIR` to it.
  An inherited `BMDS_APP_DIR` is always dropped, so a program outside a folder that one started
  doesn't see its parent's. A `#!/bin/sh` script run by the built-in shell doesn't go through the
  loader and gets none (the shell runs it itself).
- **`apps`** lists each application (command, id, program) and each problem; `apps rescan`
  scans again. The log says how many are installed, and each problem, after each scan that
  changed something.
- **Tested on the board:** a folder made by hand was found within a second; its command ran from
  `/apps` and by name on the `PATH`; `BMDS_APP_DIR` was set started and through `execve`, an
  inherited wrong one replaced, none outside a folder; removing the folder (and the root) emptied
  `/apps`.
- **`BMDS_APP_ID`** (the user's choice, 2026-10-14, over the program passing its own): set with
  `BMDS_APP_DIR`, from the installed application's id (`Apps::idOf()`, the folder compared without
  case), else the folder's name without `.app` (a folder outside `apps/path`) when that is a valid
  name. Dropped when inherited, as `BMDS_APP_DIR` is.
- **`bmds/app.h`** (the SDK's `libbmds.a`, `crt/app.c`): `bmds_app_dir()`, `bmds_app_id()` (NULL
  outside an application), `bmds_app_data_dir(buf, size)`: `$HOME/.local/share/ID` (`HOME` unset:
  `/home`), made with the folders above it; 0, or -1 with `errno` `ENOENT` (not an application),
  `ERANGE`, or `mkdir()`'s. The example `appinfo` prints them and counts its runs in its data
  folder. Tested on the board: installed with `X-BMDS-Id=infotest` in `info.app`, run by name, by
  `/apps` and by path: id `infotest`, `/home/.local/share/infotest` made, the count kept; a wrong
  inherited `BMDS_APP_ID` replaced; outside a folder, none; a `loose.app` in `/tmp` got `loose`.

## 12. As built: step 3, the launcher (2026-10-14)

- **Public APIs only** (built-in applications aren't special): the panel doesn't ask `Apps`; it
  reads `/apps`. Each link leads to a program; the `NAME.app` folder holding it (at any depth:
  `Exec=bin/x`) has the `app.desktop`. The entry is named `ID.desktop` (the id as the system's:
  `X-BMDS-Id`, else the folder's name), so an entry of that name in an earlier directory
  overrides or hides it (`Hidden=true`), as §4 says. Its `Exec`'s program is replaced by
  `/apps/COMMAND`, its arguments kept: launched, the loader sets `BMDS_APP_DIR` and `BMDS_APP_ID`.
- **`panel/launcher/applications`** defaults to `/cfg/applications;/apps;/sd0/p0/share/applications`
  (the user's entries, then the folders, then packages'). `/apps` in the list means the installed
  applications.
- **Kept current:** the panel watches the launcher's directories (`/apps` too: its watches are
  told when the set changes). A change reads the launcher again, or, under an open menu, when it
  next opens. Before, a directory missing at start was read when the menu next opened, and
  nothing else was.
- **Tested on the board:** `surfdemo` installed as `surf.app` (`Exec=surfdemo -d -t 4`,
  `Categories=Game`) while the panel ran: in the launcher within a second; chosen by the keyboard
  it ran as `/apps/surf -d -t 4`; the folder removed, gone from the launcher. Not tested on the
  board: an override in `/cfg/applications` (the same first-wins check as before).
- **Not yet:** an `Icon=` relative to the folder (a theme's name or an absolute path works; without
  one the program's own icon notes are used, `gui.md` §4j); Open With by `MimeType=` (step 4).

## 13. As built: step 4, the file browser and Open With (2026-10-14)

- **`Apps::isFolderName()`** (`NAME.app`, any case) is the one test: the loader, the panel and
  `FolderItemModel` use it. A folder is an application's by its name; one that can't run (no
  usable `app.desktop`) opens as a folder.
- **The folder window:** an application's folder shows with the theme's
  `application-x-executable` icon and the type "Application". Open (double click, Enter) runs it:
  its `app.desktop` with `Exec`'s program the folder's (`readAppFolder()`, shared with the
  launcher's `/apps` reading); it needn't be installed (a folder outside `apps/path` runs too,
  `BMDS_APP_ID` from its name). **Show Contents** (File menu and the item's menu, when one
  application folder is selected) goes into it; Open in New Window shows its contents in a new
  window, as for any folder. The tree shows it as a folder.
- **The desktop:** an application folder there shows the program icon and opens (runs) the same
  way (`FolderActions::open()`). Not tried on the board (it would have meant the user's Desktop).
- **Open With:** an id in `mimeapps.list` (`ID.desktop`) finds an installed application in the
  `/apps` source of `panel/launcher/applications`. **`MimeType=`** is now read (`DesktopEntry`: a
  long list is cut at its last whole type, not refused) and is the fallback when `mimeapps.list`
  names nothing: the type, then `text/plain` for text, each by `mimeapps.list` and then by
  `MimeType=` (the launcher's directories in order, the first of a name), and last
  `application/octet-stream` by `mimeapps.list` only. A `Hidden=true` entry in an earlier
  directory now stops `openWith()`, as it hides the launcher entry.
- **Tested on the board:** `surfdemo` as `surf.app` (`Exec=surfdemo -d -t 4 %f`,
  `MimeType=application/pdf;`): in `/sd0/p0/apps` with the application icon; Enter ran
  `/sd0/p0/apps/surf.app/surfdemo -d -t 4`; Show Contents (by the arrows, and by its mnemonic
  typed) went into the folder; a `.pdf` in `/tmp` (no line for it in `mimeapps.list`) opened as
  `/apps/surf -d -t 4 /tmp/t.pdf`.
- **Not yet:** the application's own icon in the file browser and on the desktop (the generic
  one now; the launcher already shows a program's icon notes); an Open With submenu.
