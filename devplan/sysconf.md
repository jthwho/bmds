# System configuration: the registry, `sysconf`, and `/home` (design, 2026-10-07; steps 1-3 built, step 4 designed)

## 1. Goal

The user's requirements:
- **A registry of system settings,** read and changed through an API (`SysConf`) that any program
  may use. The built-in program `sysconf` is its command-line face.
- **Every parameter has:**
  - a hierarchical key (`wm/palette/desktop`);
  - a type (int, string, color, ...), so values are checked and a settings editor can be written
    later;
  - a default. `sysconf setdef KEY` goes back to it.
- **Changes don't persist unless saved:** `sysconf set home /sd0/p0/home` lasts until the reboot;
  `sysconf save home` keeps it. Saved values live on `/boot`.
- **Listeners:** a program can be told when a value changes.
- **`/home`:** the VFS shows `/home` as a link to the directory the registry's `home` names. It is
  not driven by the environment. `/boot/autoexec.sh` sets `home` to the card's `home` directory
  when there is a card, without saving it.
- **The system's settings move into the registry,** out of `/boot/config/*.conf`.

## 2. Parameters

- **Keys:**
  - Components separated by `/`. Each component is lowercase letters, digits and `-`. At most 63
    bytes in all.
  - The first component names the owner: `wm/`, `terminal/`, `panel/`, `clipboard/`, `clock/`.
    `home` has no owner.
  - Keys are part of the userspace interface once shipped (Prime Directive 1): never renamed or
    retyped, only added.
- **Types** (each with a canonical text form, which is what `get` prints, `set` takes and the
  store holds):

| Type | Text form | Checked |
|---|---|---|
| `bool` | `true` / `false` | also `yes`, `no`, `on`, `off`, `1`, `0` on input |
| `int` | decimal | within the declared minimum and maximum |
| `string` | UTF-8 | its declared maximum length |
| `color` | `RRGGBB` (hex) | also `#RRGGBB` on input |
| `enum` | one of the declared words | |
| `path` | an absolute path | normalized; an optional check (below) |
| `paths` | absolute paths separated by `;` | each one as `path` |

- **A declaration** (rodata, no constructor) holds the key, type, default (canonical text), a
  one-line description, the limits (`int` minimum and maximum, `string` length, `enum` words), and
  an optional check:
  - A function called on `set`, which may refuse a value with an error. `home` uses it: the path
    must be an existing directory on a mounted filesystem.
  - The registry passes values to the code that uses them only after the type and the check have
    passed.
- **Built-in parameters** are declared where they are used, in a linker section
  (`BMDS_SYSCONF(...)`, as `BMDS_SHELL_COMMAND` registers commands). The registry finds them all at
  boot, so no component knows another's keys.
- **Programs' own parameters:**
  - A program loaded from the card declares its keys at run time, through the same API. Up to
    `MaxRuntime` keys (64), kept until reboot.
  - Saved values for keys nobody has declared yet are kept in the store and applied when the key is
    declared. A program's settings therefore persist although the program isn't built in.

## 3. Values and their states

Each parameter has its **current** value and, separately, possibly a **saved** value.
- **At boot:** current = saved if there is one, else the default.
- **`set KEY VALUE`:** current = VALUE (checked first). The store is untouched.
- **`setdef KEY`:** current = the default. The store is untouched (as with `set`).
- **`save KEY`:**
  - The store gets the current value.
  - If the current value is the default, the key is removed from the store instead, so a later
    release's new default applies to it.
  - `save` with no key saves every key whose current value differs from the store.
- **`revert KEY`:** current = the saved value, or the default if none.
- **What `list` shows:** a key's value, plus a mark when it differs from the store (`*`: changed,
  not saved) or when it is the default (` `).

## 4. Persistence

- **The store:** `/boot/sysconf`. One `key=value` line per saved key, sorted, with `\` escapes
  for newlines and backslashes, and a comment line naming the format. It is small (a few KB).
- **Writes:** it is rewritten whole at each `save`. On littlefs a file rewritten in place keeps its
  old contents until the close commits the new ones (`storage.md` §13c), so a power cut leaves
  the old store or the new one, never half. The ordered write cache (§13d) keeps that.
- **Reads:** loaded once, in the boot stage after `/boot` mounts, before anything reads a
  setting.
  - A line with an unknown key is kept for a later declaration (§2).
  - A line whose value no longer passes its type is ignored and logged. The default applies, and
    the line stays until the key is next saved.
- **Why not SQLite** (the user suggested it, and SQLite will likely come for applications' file
  formats):
  - The registry is in the kernel, which every boot needs. SQLite's own figures
    (sqlite.org/footprint.html, 2023) are 590 to 750 KB compiled with `-Os` on the platforms it
    lists, larger with optional features. That would grow an 892 KB firmware by two thirds or
    more, to store a few dozen lines.
  - Its journal would also add writes per save, on top of littlefs's own atomicity.
  - SQLite fits better as an SDK library that applications link (on the card, costing the
    firmware nothing). The store's format is private to the registry, so moving it to SQLite later
    is possible without changing the API.
  - This is the user's call (§10).

## 5. Listening for changes

- **In the kernel (C++):** a `SysConf::Watcher` the listener owns, so there's no allocation. It
  registers a key prefix (`wm/palette/`, or `""` for everything).
  - A change adds the key to the watcher's small ring and releases its `WaitList`. On overflow, a
    flag means "anything may have changed".
  - The listener waits on the list, or polls `next()` after a wake-up from its own event loop.
  - Watchers are told after the value changed, outside the registry's lock, so a listener may read
    the new value at once.
- **In the GUI toolkit:** `GuiApplication::watchConfig(prefix, fn, arg)` calls `fn` on the
  application's loop, as `setWindowsChanged()` does. The window manager, terminal and panel use
  it to apply a new palette or colors at once.
- **In userspace (SDK, a later step):** a watch is a file descriptor that becomes readable when a
  matching key changes, and `read()` returns the changed keys, one per line. It works with the
  existing readiness waits.

## 6. The API

- **Kernel** (`SysConf`, a namespace like `Vfs`, thread-safe):
  - `get(key, String &out)`, plus typed forms `getInt()`, `getColor()`, `getBool()`
  - `set(key, value)`, `setDefault(key)`, `save(key)`, `saveAll()`, `revert(key)`
  - `info(key)`: type, default, limits, description, state
  - `list(prefix, callback)`
  - `declare(const Param &)`, for run-time keys
  - `Watcher`
- **SDK** (C, `bmds_sysconf_*`, new syscalls), a later step, with an `ABI:` line:
  - get, set, setdef, save, revert, info, declare, list (into a buffer), watch (a descriptor).
  - Boundary structs start with `uint32_t size`.
- **`sysconf`** (`/bin/sysconf`, a built-in program on the kernel API, as the terminal uses the
  toolkit's). Its output is a shell interface, frozen once shipped:

```
sysconf get KEY                 the value
sysconf set KEY VALUE           checked; not saved
sysconf setdef KEY              back to the default; not saved
sysconf save [KEY]              KEY, or every changed key
sysconf revert KEY              back to the saved value (or the default)
sysconf list [PREFIX]           KEY TYPE VALUE, a * when not saved
sysconf info KEY                type, default, limits, description, saved value
```

## 7. `/home`

- **The parameter:** `home`, type `path`, default `/boot/home`.
  - Its check: an existing directory on a mounted filesystem. `sysconf set home /sd0/p1/x` with
    no card is refused (`NotExist` or `NoMedia`).
  - `/boot/home` is created at boot if it is missing.
- **The VFS shows `/home` as a symbolic link** to the registry's `home`:
  - `ls -l /` shows `home -> /sd0/p0/home`, and `readlink /home` gives it.
  - It can't be removed, renamed or written over (`PermissionDenied`, as in `/dev`).
  - It is resolved at each lookup, from the current value. The VFS watches `home`.
- **When `home` can't be reached:** if the filesystem holding it goes (the card pulled), `/home`
  points to the default, `/boot/home`, until it comes back. The value itself isn't changed, so the
  card's home returns with the card, and a line in the log says so each way. This is the fallback
  agreed earlier.
- **The environment follows:** `/boot/profile` sets `HOME=/home`, and so does the shell's
  default. `cd ~`, the panel and the terminal reach the right place through the link.
- **Single user:** `/home` *is* the user's directory, not a directory of users. If several users
  ever come, it becomes `/home/<user>`.
- **At boot:** a saved `home` on a card that isn't mounted yet shows `/boot/home` until the card
  mounts, then the card's home (the live switch). The user's `autoexec.sh` instead sets `home`
  after `waitmount`, without saving: either works.

## 8. What moves into the registry

System settings become parameters. Data that is a list or a document stays a file, in the user's
home where it belongs to the user (the two-place lookup agreed earlier: `/home/.config/NAME`,
else the system default in `/boot/config/NAME`).

| Today | Becomes |
|---|---|
| `win color` (the 23 palette roles, not saved anywhere) | `wm/palette/<role>` (`color`), e.g. `wm/palette/desktop` |
| `terminal.conf` `[Colors]` Text, Background, Cursor | `terminal/colors/text`, `.../background`, `.../cursor` (`color`) |
| `terminal.conf` `[Clipboard]` AllowWrite, AllowRead | `terminal/clipboard/allow-write`, `.../allow-read` (`bool`) |
| `panel.conf` `[Panel]` Edge, Items, IconTheme | `panel/edge` (`enum` top, bottom), `panel/items` (`string`), `panel/icon-theme` (`string`) |
| `panel.conf` `[Launcher]` Label, Applications, Submenus | `panel/launcher/label`, `panel/launcher/applications` (`paths`), `panel/launcher/submenus` (`int`) |
| `panel.conf` `[Windows]` MaxWidth, `[Clock]` Format | `panel/windows/max-width` (`int`), `panel/clock/format` (`string`) |
| `panel.conf` `[Desktop]` Menu, Icons, MimeApps | `panel/desktop/menu` (`bool`); `Icons` becomes `/home/Desktop`, and `MimeApps` the two-place lookup of `mimeapps.list` |
| `clipboard.conf` `[Limits]` Bytes | `clipboard/limit` (`int`) |
| `clipboard.conf` `[Convert]` (converter programs) | stays a file (a list) |
| `wm.conf` `[Menu]`, `[Keys]` (labels and commands, in order) | stay a file (a list), `/boot/config/wm.conf` |
| The kernel's time zone (`clock zone`, `CONFIG_BMDS_TIMEZONE`) | `clock/zone` (`string`, checked as a TZ string or zone name) |
| The zone database (`clock tzdata PATH`) | `clock/tzdata` (`path`) |
| `desktop.conf` (where icons were put), `mimeapps.list` overrides | files in `/home/.config/` (the user's) |
| `/boot/desktop` | `/home/Desktop` (`/boot/home/Desktop` filled from the defaults) |

- **The `.conf` files whose settings all moved go away.** The defaults installer stops shipping
  them, and a running system reads the registry. Programs read their keys at start and watch them
  for changes.
- **Not moved:** `/boot/profile` and `/boot/autoexec.sh` (scripts), `.defaults` (the installer's
  own record).

## 9. Steps

1. **The registry.**
   - In `bmds_core` (pure, host-tested): the types, parsing and canonical forms, and the store's
     format.
   - In a new component `bmds_sysconf`: declarations, values, watchers, load and save, the boot
     stage, and `/bin/sysconf`.
   - Host tests.
2. **`/home`:**
   - the VFS link, the `home` parameter and its check;
   - `/boot/home` with `Desktop`, from the defaults;
   - `HOME=/home`.
3. **The settings move** (§8), one owner at a time, with `watchConfig()` in the toolkit:
   - the window manager's palette;
   - the terminal;
   - the panel (with `/home/Desktop` and the two-place lookup);
   - the clipboard;
   - the clock.
4. **The SDK's `bmds_sysconf_*` syscalls** and watch descriptors (an `ABI:` step).

Board checks at each step: `sysconf` round trips, a saved value surviving a reboot and an unsaved
one not, `/home` following a card pulled and put back, the palette changing live.

## 10. Decisions (the user's, 2026-10-07)

1. **The store's format:** a text file (§4).
2. **Lists stay files:** the desktop menu, the key bindings and the clipboard's converters.
3. **`setdef` and `set`:** both leave the store untouched until `save`.
4. **`save` with no key:** saves every changed key.

## 11. As built (2026-10-07, steps 1-3)

- **Where things are:**
  - `Config` in `bmds_core` (`configtypes.h`): the types, canonical forms and the store's lines.
    `Config::ColorText<0xRRGGBB>` makes a default from a `constexpr` color.
  - The registry in a new component, `bmds_sysconf`, portable and host-tested. Its boot stage
    (`sysconfinit.cpp`, the board's) logs what the portable part counts, since logging is
    board-only.
  - The boot stage `sysconf` runs after `boot` (`/boot` mounted) and before `clipboard`.
- **One deviation:** `sysconf` is in `bmds_shell` (`sysconfcommand.cpp`), not in
  `bmds_sysconf`. The shell's `clock zone` and `clock tzdata` set `clock/zone` and
  `clock/tzdata`, so the shell depends on the registry, not the other way round.
- **`/home`:**
  - The VFS gained `Vfs::setHome(target, fallback)` and `setHomeReport()`. Paths at or below
    `/home` are rewritten as they are normalized, at each entry point.
  - As the last component of `lstat`, `readlink` and the changes, `/home` is the link itself:
    `remove` and `rmdir` give `PermissionDenied`, `mkdir` and `symlink` give `Exists`.
  - Nothing can be mounted at `/home`.
  - A home new to its directory gets `/boot/home/Desktop`'s files once.
- **The parameters (60 on the board):**
  - `home`
  - `clock/zone`: default `CONFIG_BMDS_TIMEZONE`, checked by the new
    `Clock::checkTimeZone()`.
  - `clock/tzdata`: default `CONFIG_BMDS_ZONEINFO_PATH`. The storage service no longer sets it.
  - `wm/palette/<role>`: 23, defaults the palette's; `desktop` defaults to
    `CONFIG_BMDS_DESKTOP_COLOR`.
  - `terminal/colors/<name>`: 19 (`text`, `background`, `cursor`, `color0` to `color15`).
  - `terminal/clipboard/allow-write` and `allow-read`.
  - `panel/...`: 12.
  - `clipboard/limit`.
- **Who follows changes live:**
  - The window manager: `win start` builds its palette from the registry and watches
    `wm/palette`.
  - The terminal: it watches `terminal`; colors apply at the next drawing, under the window
    manager's lock, and it asks for that drawing.
  - The clipboard: it watches `clipboard/limit`.
  - The clock: `clock/zone` and `clock/tzdata` are applied at boot and on change.
  - **Not the panel yet:** it reads its parameters when it starts, so a change takes effect
    when it starts again. `GuiApplication::watchConfig()` (§5) waits for it, its first user.
- **The commands kept their syntax** (until §13 removed these forms):
  - `win start RRGGBB` and `win color ROLE RRGGBB` set `wm/palette/...`.
  - `win color terminal-text RRGGBB` sets `terminal/colors/text`.
  - `clock zone` and `clock tzdata` set the clock's keys.
  - None of them save.
  - The window manager's resources (`watchResources()`), which `win color terminal-*` used, now
    have no user.
- **Files:**
  - Gone: `boot/config/terminal.conf`, `boot/config/panel.conf` (`panel -c FILE` still reads a
    file in that format), and `clipboard.conf`'s `[Limits]` (the clipboard reports its lines as
    not taken, rather than reading a limit sysconf then replaces).
  - **`/boot` cleaned:** the installer never removes a file, so a board that had the old
    defaults keeps them. The one board's were removed by hand (`panel.conf`, `terminal.conf`,
    `config/desktop.conf`, `/boot/desktop`) with their `.defaults` lines, and `clipboard.conf`
    and `mimeapps.list` reinstalled with their new text. A list of retired defaults for the
    installer to remove (when unchanged) waits for a second board.
  - `boot/desktop` became `boot/home/Desktop`.
  - `desktop.conf` is written to `/home/.config/`.
  - `mimeapps.list` is `/home/.config/`'s if there is one, else `/boot/config/`'s.
  - `/boot/profile` and the shell set `HOME=/home`.
  - `boot/autoexec.sh` is the user's: with a card, `mkdir -p /sd0/p0/home` and
    `sysconf set home /sd0/p0/home`, not saved.
- **Tests:** host (`tests/host/sysconf.cpp`):
  - the types and the store's format;
  - set, setdef, revert, save and a reboot (`initForHost` again), and the default saved as no
    line;
  - a value its type refuses in the store, and a saved line kept for a key declared later;
  - list by prefix (`wm/palette2` isn't under `wm/palette`);
  - watchers (prefixes, repeats not queued twice);
  - the VFS's `/home` with a card unmounted and mounted again.
- **On the board:**
  - **Boot:** `/home` followed the card's `home` once `autoexec.sh` set it, and the card's new
    home got the Desktop launcher.
  - **Saving:** a saved value (`home=/boot`) was there after a reboot and `revert` went back to
    it. Saving the default removed the line.
  - **Refusals:** a missing directory was refused for `home`, and `clipboard/limit 10` as out
    of range.
  - **Unmounting:** `umount /sd0/p0` moved `/home` to `/boot/home`, and `mount` moved it back.
  - **Live changes:** the desktop's and the active frame's colors, and a terminal's background,
    changed at once.
  - **A saved palette color:** it came back after a reboot, the window manager having started
    from the registry.
  - **The clock:** `clock zone` with a TZ string set `clock/zone`, and an unknown name was
    refused.
  - **The desktop:** dragging an icon wrote `/home/.config/desktop.conf` on the card.
- **Size:** the firmware is 908,736 bytes, 16.6 KB more than before (the registry, 60
  declarations, `/home`, the command).
- **Not done (step 4):** the SDK's `bmds_sysconf_*` syscalls and watch descriptors.

**Follow-up (2026-10-07, the user's notes):**
- **The prompt's `~`:** the shell's `\w` and `\W` didn't show `~` with a card, because `cd`
  keeps the physical path (`/sd0/p0/home`) while `HOME` is `/home`. The prompt now also takes
  `$HOME`'s real path as home.
  - `HOME` stays `/home`, not the registry's path. It is fixed, so programs already running stay
    right when `home` changes. Programs (BusyBox) expect a `HOME`.
  - The board's `/boot/profile` was older (`HOME=/boot`); the installer doesn't replace a
    default once installed.
- **A home whose filesystem goes becomes the default:**
  - A thread (`home`) wakes at each mount and unmount (`Vfs::mountWaiters()`). If `home` isn't
    a directory any more, it sets the default (not saved) and remembers the value. When that
    directory is back, it sets it again.
  - Setting `home` meanwhile forgets the remembered value.
  - The VFS's own fallback (§7) still covers the moment in between.
  - **At boot:** a saved home on a card shows the default until the card mounts, then comes back.
- **Variables whose truth is the registry** (the user's request):
  - **`HOME`:** the shell sets and exports `HOME=/home` unless it inherits one.
  - **`PATH`:** a new parameter, `shell/path` (61 now), is a new shell's `PATH` unless it inherits
    one. Shells running keep theirs, as with any environment.
  - `/boot/profile` sets neither now; it holds only comments, for the user's own settings.
  - **`TZ`:** the loader used to add the kernel's zone to every program's environment, built-ins
    included. The login shell thus kept a boot-time `TZ=UTC0` that hid later `clock/zone` changes
    from `date` in it and from everything it started.
  - Now only programs loaded from files get it (musl reads `TZ`), with the zone as it is when
    they start. Built-in programs ask the kernel, so they follow `clock/zone` live.
  - Setting `TZ` in a shell still overrides the zone for it, as in POSIX.
- **Checked on the board:**
  - `HOME=/home` with no profile line.
  - `~` and `~/Desktop` in the card's home.
  - A saved `shell/path` in the login shell after a reboot.
  - `TZ` empty in the login shell, and `date` following `clock zone` and `sysconf setdef
    clock/zone` at once.
  - `umount /sd0/p0`: `home` became `/boot/home` ("isn't there"). `mount`: it was
    `/sd0/p0/home` again ("is back").
- **The default `autoexec.sh`** (2026-10-07, the user's and this review's):
  - **Home:** with a card, the card's (`mkdir -p /sd0/p0/home`). Without one, `/boot/home`, set
    explicitly so a home saved elsewhere doesn't linger. Neither is saved.
  - **The desktop:** the card's `autoexec.sh` starts it if the card has one. Otherwise a built-in
    desktop (display, window manager, panel) starts, so a card without the script no longer
    leaves the screen dark.
  - **`/home/autoexec.sh`,** if there is one, runs last: the user's own startup, card or not.
  - **Checked on the board** with a card: the defaults installer put the new script in (the old
    one and its `.defaults` line removed first). `home` was the card's, the card's script started
    the panel, and a test `/home/autoexec.sh` ran.
  - **Not tried:** booting without a card, and so the panel without the card's icon theme.

## 12. Step 4: ids, the SDK's calls and watch descriptors (design, 2026-10-07; not built)

### 12.1 Ids (the user's suggestion)

- **A parameter has an id:** a small number naming it for the rest of the boot. A program finds a
  key's id once (`find`), then gets, sets and is told of changes by id. Strings are compared only
  in `find`, `list` and when a watch starts.
- **Ids aren't in the ABI:** they are handles for this boot, never numbers to write into a
  program.
  - The built-ins' order is the linker section's, which changes from one firmware to the next,
    and run-time keys get theirs as they are declared.
  - A program that kept an id across a reboot would read another key. Keys stay the interface
    (§2); ids are how a running program holds one.
- **The registry has them already:** a watcher's ring holds entry numbers (`uint16_t`), which
  `next()` turns back into keys. Step 4 makes them public: `SysConf::Id`, `find(key)`, and `get`,
  `set`, `setDefault`, `revert`, `save` and `info` by id. The forms taking a key stay (the shell,
  and code that reads a key once).
- **Finding:** an index of the entries sorted by key, made at boot (2 bytes an entry, internal
  RAM: 250 bytes for 61 built-ins and 64 run-time keys), and a binary search. A key declared at
  run time is inserted into it.
- **Watching without strings:** a watch's prefix is matched once, when it starts, into a bit mask
  over the entries (`MaxEntries` bits: 16 bytes for 128 entries, in the watcher). A key declared
  later is matched against the watchers' prefixes once, when it is declared. A change is then one
  bit test per watcher. The ring holds ids, as now.
- **The kernel's own users move to ids:** the window manager's palette (23 keys) and the
  terminal's colors (19) find their ids when they start, then index tables by id instead of
  comparing keys at each change.

### 12.2 The calls (slots 59-65, appended)

All take and return ids as `int32_t`: an id is >= 0, an error negative (`-BMDS_E...`). Values are
the canonical text forms (§2), at most `BMDS_SYSCONF_VALUE_MAX` (1024) bytes; keys at most
`BMDS_SYSCONF_KEY_MAX` (63).

| Slot | Call | What |
|---|---|---|
| 59 | `int32_t sysconf_find(const char *key)` | The key's id, or `-ENOENT` |
| 60 | `int32_t sysconf_get(int32_t id, uint32_t what, char *out, uint32_t size)` | A text of the parameter, NUL-ended, its length returned (`-ERANGE` if @p size is too small): `BMDS_SYSCONF_VALUE`, `_DEFAULT`, `_SAVED` (`-ENOENT` if none), `_KEY`, `_HELP`, `_CHOICES` |
| 61 | `int32_t sysconf_info(int32_t id, bmds_sysconf_info_t *info)` | Its numbers: `size`, type, min, max, flags (`BMDS_SYSCONF_IS_DEFAULT`, `_IS_SAVED`, `_CHANGED`: differs from the store, `_RUNTIME`: declared by a program) |
| 62 | `int32_t sysconf_set(int32_t id, uint32_t op, const char *value)` | `BMDS_SYSCONF_SET` (@p value), `_SETDEF`, `_REVERT`, `_SAVE`; `_SAVE` with id -1 saves every changed key. Errors: `-EINVAL` (not of the type), `-ERANGE` (a limit), the check's (`-ENOENT`, `-ENOTDIR` for `home`), `-EIO` |
| 63 | `int32_t sysconf_next(const char *prefix, int32_t after)` | Listing: the id of the first key under @p prefix after id @p after in key order (-1: from the start), or `-ENOENT` at the end |
| 64 | `int32_t sysconf_declare(const bmds_sysconf_param_t *param)` | A program's own key (§2): its id. Declaring a key again with the same type and limits gives the same id (a program started twice); otherwise `-EEXIST` |
| 65 | `int32_t sysconf_watch(const char *prefix, uint32_t flags)` | A new descriptor (`BMDS_O_CLOEXEC` taken) |

- **The watch descriptor:**
  - `read()` gives whole `uint32_t` records: the id of each key changed, or
    `BMDS_SYSCONF_OVERFLOW` (0xffffffff): more changed than the ring held, so read again what
    matters. A size not a multiple of 4 is `-EINVAL`.
  - It blocks when nothing changed, unless the descriptor is non-blocking (`fd_flags`).
    `io_poll` reports `BMDS_POLL_IN`. A program's event loop waits on it with everything else.
  - Closing it ends the watch. `dup` and inheritance share it, as for a pipe.
  - A repeat of an id not yet read isn't queued twice (as now).
- **`bmds_sysconf_param_t`:** `size`, key, type, default, help, min, max, choices. The kernel
  copies the strings (PSRAM), since the program's memory goes when it ends and the key stays until
  reboot. A program's key can't have a check function: only its type and limits.
- **The SDK header** `bmds/sysconf.h`: the wrappers, the constants and structs, and inline
  parsers of the canonical forms (`bmds_sysconf_to_int`, `_to_bool`, `_to_color`).
- **Who may change what:** every program may set and save every key. BMDS has one user, as the
  shell's `sysconf` already can.
- **Frozen once shipped:** the slots, the structs, the constants, the record format and the type
  numbers (`Config::Type`'s order, now an interface: `bool` 0 to `paths` 6).

### 12.3 The toolkit

- **`GuiApplication::watchConfig(prefix, fn, arg)`:** a `SysConf::Watcher` whose notify posts to
  the loop (`post()`), and `fn(arg, id)` called on the loop's thread for each id, or with
  `SysConf::AnyId` after an overflow.
- **The panel follows its keys live:** `panel/edge` moves it, `panel/items` and the launcher's
  keys rebuild the row, `panel/icon-theme` reloads the icons, `panel/clock/*` reconfigures the
  clock, `panel/desktop/menu` turns the desktop's menu on or off.

### 12.4 Order

1. Ids in the kernel API (`find`, by-id forms, the sorted index, watchers' masks); the window
   manager and terminal by id. Host tests: ids stable through the boot, `find` for built-in and
   declared keys, masks for keys declared after a watch.
2. The slots, `bmds/sysconf.h` and the watch descriptor (`ABI:`), with an SDK example program
   and the SDK's ABI lock test extended.
3. `watchConfig()` and the panel live.

### 12.4a As built: step 1, ids in the kernel API (2026-10-07)

- **`SysConf::Id`:** a strong `enum class` over the entry number, so a plain integer isn't taken
  by mistake, plus `NoId`.
  - `find(key)` gives the id and `keyOf(id)` the key.
  - `get`, `getInt`, `getBool`, `getColor`, `set`, `setDefault`, `revert`, `save` and `info`
    each take an id as well. The forms taking a key find it and call them.
  - `declare()` returns the id: the same one when a key is declared again with its type.
- **The sorted index existed already** (the registry's `order`, 2 bytes an entry). It stays in
  PSRAM with the entries rather than moving to internal RAM as §12.1 said: `find` runs once per
  key now, so it isn't hot.
- **Watchers:** a mask of `MaxEntries` (256) bits, 32 bytes a watcher, not §12.1's 128. The
  registry holds at most 256 entries: built-ins plus `MaxRuntime`, the run-time room shrinking if
  the built-ins ever pass 192.
  - The mask is filled when the watcher starts. `declare()` sets the bit in each started
    watcher whose prefix the new key is under.
  - A change tests one bit per watcher. `next()` gives ids, and `detail::keyOf()` is gone.
- **The window manager** finds its 23 roles' ids once. On a change it reads only the roles whose
  ids its watcher queued, into its current palette, and the whole palette after an overflow.
- **The terminal** finds its 19 colors' ids, where each goes in its colors, and the two
  clipboard switches once. It reads the colors by id at the next drawing.
- **Tests (host):** `SysConf: ids`. Ids are the same each time and name their key; `NoId` and
  unknown keys are refused. Set, save, setdef, revert, info and the typed gets work by id. A key
  declared after two watchers started is told only to the one whose prefix it is under. A key
  that sorts before every other doesn't change the others' ids.
- **On the board:** `wm/palette/desktop`, `wm/palette/active-frame` and
  `terminal/colors/background` changed the desktop, the active frame and an open terminal at
  once.
- **Size:** the firmware is 909,424 bytes, 1,232 more.

### 12.4b As built: step 2, the SDK's calls (2026-10-07)

- **Slots 59-65** as in §12.2, in `sdk/include/bmds/sys.h` with the other slots rather than a
  header of their own. The names in full:
  - `BMDS_SYS_SYSCONF_FIND`, `_GET`, `_INFO`, `_SET`, `_NEXT`, `_DECLARE`, `_WATCH`, and
    `bmds_sysconf_find()` and the rest.
  - `BMDS_SYSCONF_FIELD_*` (`get`), `BMDS_SYSCONF_OP_*` (`set`), `BMDS_SYSCONF_TYPE_*`,
    `BMDS_SYSCONF_IS_DEFAULT`, `_IS_SAVED`, `_CHANGED`, `_RUNTIME`, `BMDS_SYSCONF_ALL` (-1, for
    `OP_SAVE`), `BMDS_SYSCONF_OVERFLOW`.
  - `bmds_sysconf_info_t` (24 bytes) and `bmds_sysconf_param_t` (32).
  - All in the ABI lock (`abilock.c`), and `process.cpp` asserts that the types' numbers and the
    lengths are the registry's.
- **Differences from §12.2:**
  - No non-blocking watch: `fd_flags` has no such flag, so a read waits, and a program that
    mustn't wait uses `io_poll` (or musl's `O_NONBLOCK`, which is built on it).
  - The inline parsers (`bmds_sysconf_to_int` and so on) were left out. A value is text in its
    canonical form, which `strtol` and `strcmp` read, and fewer inline functions means less to
    keep.
  - Declaring a key again gives its id whatever the new default says. Only a different type
    is `EEXIST`. The registry checks for the key before the default, so a second declaration
    with another type isn't refused as `EINVAL`.
- **Kernel side:**
  - `Process::sysconf*` check the program's arguments: keys within 63 bytes, values within
    1024, help within 255, and the struct's `size`.
  - The registry gained `SysConf::next(prefix, after)`, `Info::defaultValue`, `isDefault` and
    `runtime`, `Watcher::isPending()`, and `openWatch(prefix)`.
  - `openWatch` returns a `FileHandle` on a `Stream` that owns a `Watcher` (`sysconfwatch.cpp`).
    Reads wait on a semaphore the notification releases, interrupted by signals as a pipe's
    are; `readiness()` is the watcher's pending state. The stream frees itself when its handle
    goes, as a pipe does.
  - `bmds_loader` now requires `bmds_sysconf`.
- **The SDK example `conf`:** `conf [PREFIX]` lists, `conf -w [PREFIX]` lists and then prints
  each change, `conf KEY=VALUE...` sets, and `conf -d` declares `example/greeting`.
- **Tests (host):** `SysConf: the SDK's calls` drives a `Process`:
  - every field of `get`, and `ERANGE` for a short buffer;
  - each `set` op, with `info`'s flags following and a short `info` struct getting what fits;
  - `next` in key order under a prefix (`wm/palette` without `wm/palette2`);
  - `declare` again, with another type, a bad key or type, and a default outside its limits;
  - a watch: `io_poll`, records one at a time, a read woken from another thread, the overflow
    record after 20 changes, and close.
- **On the board** (`conf.elf` sent to the RAM disk with `bmds-run -n`):
  - It listed `display/...`. `panel/edge=left` was refused (`Invalid argument`, status 1), and
    two keys were set at once.
  - `-d` twice gave the same key, which `sysconf info` showed as a program's string of at most
    64 bytes. Setting it and listing `example` showed the new value.
  - `-w wm/palette/desktop` in the background printed the starting value, then the shell's
    `set` and `setdef`, and nothing for `wm/palette/face`.
- **Size:** the firmware is 913,000 bytes, 3,576 more.

### 12.5 Decided (the user's, 2026-10-07)

- Ids, as in §12.1: the user's suggestion; this design keeps them per boot, out of the ABI.
- §12.2's calls as designed: the user said to keep going after they were put to them.
- **One way to do a thing:** commands that set what is now a parameter lose that use, and
  `sysconf` is the only way (§13).

## 13. One way to set a thing (2026-10-07, the user's request; built)

- **The rule:** a setting that is a parameter is set only with `sysconf` (or, after step 4, the
  calls). Commands keep showing state, but they no longer set what the registry holds. That is one
  way for the user to learn and for the documentation to describe.
- **Gone** (shell interfaces, `ABI:`):
  - `win start RRGGBB` and `win color [ROLE RRGGBB]`: `wm/palette/<role>` and
    `terminal/colors/...`. `sysconf list wm/palette` lists the palette.
  - `clock zone TZ` and `clock tzdata PATH`: `clock/zone` and `clock/tzdata`. `clock` and
    `clock tzdata` still show the zone and the database.
  - `panel -c FILE`, and with it the panel's INI layer (its own settings table, the file reader,
    the built-in default layout). The panel reads its `panel/...` keys directly. An item's kind
    is its name in `panel/items`.
  - `log level` and `log on|off MODULE...`: the new `log/level` and `log/debug`. `log` shows
    them, and `log show` the history.
  - `display start`'s mode and buffering arguments: the new `display/mode` and
    `display/buffering`.
  - The window manager's resources (`setResource()`, `resource()`, `watchResources()`), which had
    no user since the terminal's colors became parameters.
- **New parameters (65 now):**
  - `display/mode` (`string`, default `720`): `720`, `1080` or `WIDTHxHEIGHT@HZ`, checked as
    `display start` reads it.
  - `display/buffering` (`enum` `single|double`, default `double`, what the default
    `autoexec.sh` used).
  - Both are read by `display start`. A change takes effect at the next `display start`, since a
    running desktop owns the display.
  - `log/level` (`enum` `info|warn|err`, default `info`).
  - `log/debug` (`string`): debug modules, `;` between, or `all`. Each name must be a module
    `log` lists. Both are applied at once and at boot, so a saved `log/debug` debugs from the
    `sysconf` stage on.
- **Not moved,** and why:
  - `term color|size`: the terminal session's, not the system's.
  - The serial console's color depth: a build setting (`CONFIG_BMDS_USJ_COLOR_*`) for a console
    that exists before the registry.
  - `wm.conf`, `clipboard.conf` `[Convert]`, `mimeapps.list`: lists (§10).
- **`waitmount` on an empty slot:**
  - Before, `waitmount /sd0/p0 3000` with no card waited the whole 3 seconds at every boot.
  - Now the VFS counts mounts on their way: `Vfs::beginSettling()` and `endSettling()`,
    nested, and `isSettling()`.
  - The storage service settles from its start until the first card check is done, and from
    each card-detect edge until that card is mounted or known not to be. `endSettling()` wakes
    the mount waiters.
  - `waitmount` gives up as soon as the path isn't mounted and nothing is settling. A card
    being probed is still waited for (up to MS).
  - A path that only a later manual `mount` would provide no longer waits; nothing used that.
- **The card's `autoexec.sh`** had `display start 720 double`, which now fails. Its line became
  `display start` (the defaults are the same mode). The repository's `boot/autoexec.sh` changed
  the same way, and the board's was reinstalled.
- **Checked on the board:**
  - The removed forms print their usage.
  - `log/level warn` and `log/debug 'Storage;Boot'` showed in `log`. An unknown module was
    refused (`NotExist`).
  - `display/mode 999` was refused; `1920x1080@60` was taken. The desktop came up from the
    defaults.
  - The panel, restarted, followed `panel/edge bottom`, `panel/items 'launcher;spacer;clock'`, a
    clock format and the launcher's label.
  - With `/sd0/p0` unmounted, `waitmount /sd0/p0 3000` failed in under 0.05 s. Mounted, it
    succeeded at once.
- **Not tried:** a boot without a card (the empty-slot case `waitmount` was changed for).
- **Size:** the firmware is 907,920 bytes, 2.1 KB less than before.
