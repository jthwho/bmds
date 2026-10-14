# Applications: one folder each

Status: **planned** (2026-10-14, with the user). Comes before the games (`games.md`), which are
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
  whoever started it: the launcher, the file browser, the shell, `execvp`. The SDK's
  `bmds_app_dir()` returns it. No system call.

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
  `bmds_app_data_dir()` makes it and returns its path.
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
`app.desktop`), the `X-BMDS-*` keys, `BMDS_APP_DIR`, `/apps` and its place in the default
`PATH`, the data directory's place. No system calls: `bmds_app_dir()` and
`bmds_app_data_dir()` are SDK library functions.

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
