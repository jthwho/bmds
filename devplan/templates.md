# Text templates: `${NAME}` and `${NAME:ARG}`

One scheme for text the system fills in: a setting's path (the screenshots'), a program's file
names, later anything named by the date, the host or the user. The OS and programs (an SDK call)
expand it the same way, so a template that works in one place works in all.

## 1. Why

Paths were written into the code (`~/Images/Screenshots/...`, the sound font, the icon themes).
The user's rule (2026-10-10): **a path is a setting with a sensible default, never a constant**,
and where a name is made from the date, the host and so on, the setting is a template.

## 2. The syntax

- **`${NAME}`**, or **`${NAME:ARG}`** where the name takes an argument. A name is `A`-`Z`, then
  `A`-`Z`, `0`-`9` or `_`. The argument is everything up to the next `}` (it can't hold one).
- **`$$`** is a `$`. Any other `$` is an error, as is a `${` without its `}`, a name not known,
  an argument to a name that takes none: a template is checked when it is set, and an error now
  leaves room for meanings added later.
- Everything else is copied as it is.

## 3. The names

| Name | Gives |
|------|-------|
| `HOME` | The user's home: `/home` (which leads where the `home` setting says). |
| `HOSTNAME` | The system's host name (`system/hostname`, else `bmds-` and the serial's last six). |
| `SERIAL` | The board's serial number (empty if it has none). |
| `DATE` | The local date, `%Y-%m-%d`; **`${DATE:FORMAT}`** formats it as `strftime` does. |
| `TIME` | The local time, `%H-%M-%S` (no `:`: FAT can't have one); **`${TIME:FORMAT}`** as `DATE`. |

`DATE` and `TIME` with a format are the same (each takes any of `formatTime()`'s conversions);
the defaults differ. One template's times are all one instant. Names are appended, never changed
or removed (a userspace interface: `docs/ABI.md`).

Examples: `${HOME}/Images/Screenshots/${HOSTNAME}_${DATE}_${TIME}.png` gives
`/home/Images/Screenshots/bmds-e2e222_2026-10-10_14-03-27.png`;
`${HOME}/logs/${DATE:%Y/%m}/run-${TIME:%H%M}.txt` gives `/home/logs/2026/10/run-1403.txt`.

## 4. The code

- **`TextTemplate`** (bmds_core, pure, host-tested): `check(text)` and
  `expand(text, values, out)`, the values (home, host name, serial, a `LocalTime`) given by the
  caller.
- **`SysConf::expand(text, out)`** (bmds_sysconf): the system's values (the host name, the
  serial, the clock's local time) into `TextTemplate::expand()`. Settings that are templates are
  `String`s checked with `TextTemplate::check()` (a `Path` would normalize the `${...}` away).
- **`bmds_expand(text, out, size)`** (slot 92): the same for programs. Returns the expansion's
  length (as `snprintf`: more than `size - 1` means it was cut), or `-BMDS_EINVAL` (a bad
  template), `-BMDS_EFAULT`.

## 5. Its users

- **`screenshot/path`**: `${HOME}/Images/Screenshots/${HOSTNAME}_${DATE}_${TIME}.png`, Print
  Screen's file (`devplan/notifications.md` §4); `screenshot` without a path saves there too.
  Ending in `.jpg`, it saves JPEGs (`devplan/images.md` §7).

## 6. Paths still in the code (to become settings)

Found 2026-10-10; each to become a setting (a template where it names by the date or host) as
its subsystem is next worked on:

- `SynthFile::DefaultFont` (`/sd0/p0/share/soundfonts/TimGM6mb.sf2`).
- `IconTheme::ThemesDir` (`/sd0/p0/share/icons`) and the user's `/boot/icons`.
- The player's `KeptDir` (`/home/.config/mplayer`) and the panel's `/home/.config/*` files (one
  `XDG_CONFIG_HOME`-like setting, `${HOME}/.config` by default).
- The panel's `DefaultDesktop` (`/home/Desktop`, the folder window's too).
- Files under `/boot` (`/boot/config/*`, `/boot/profile`, `/boot/autoexec.sh`,
  `/boot/sysconf`) stay: they are where the settings themselves are found.

## 7. ABI impact

- **Call:** `expand` (slot 92), appended.
- **The syntax and names** (§2, §3): frozen once shipped, names appended.
- **Setting:** `screenshot/path`.
- **Shell:** `screenshot`'s path becomes optional (its output unchanged).
