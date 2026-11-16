# Other people's packages: sources, keys, the SDK, and `bmds-apps`

Planned 2026-11-15 with the user. Builds on `packages.md` (§6 keys, §8 repositories, §21-§23
sources and fetching, §25 publishing) and `controlpanel.md`.

## 1. Goal

Anyone can publish packages for BMDS, and a user can add their repository, trust it, and install
from it, without editing a setting by hand or copying a key file onto the card. A real
repository, **`bmds-apps`**, shows how: a Minesweeper game to start with (later, the non-core
apps moving out of this repository), built with the SDK from a release, signed with its own key,
and published as GitHub release assets. Its README is the walkthrough for anyone else.

## 2. Decisions (the user's, 2026-11-15)

1. **No store yet** (`packages.md` §18 stays planned). This plan is sources: list, add, remove,
   from the shell and from the control panel; installing stays `pkg install NAME`.
2. **Trust on first use:** adding a source fetches the key its index is signed with from the
   repository itself, shows it (id, comment, where it came from) and asks. Good enough.
3. **The repository is `jthwho/bmds-apps`** (`~/src/bmds-apps`), not one per app. Minesweeper
   first. It becomes one of BMDS's default sources.
4. **The SDK is a release asset:** `tools/release` builds `bmds-sdk-X.Y.Z.tar.gz`; `bmds-apps`
   builds against it.
5. **`bmds-apps` has its own key**, built into the firmware, and the key setup must be clear
   enough that the repository serves as the example for others (§3).

## 3. Keys

### 3.1 What exists, and a problem

The firmware's built-in keys are `keys/dev.pub` (in the dev mode) and `keys/trusted.pub`, and
**both kinds are accepted for firmware updates** (`bmds_update_keys()`, `UpdateKeys::accepted()`).
A key added on a board (`pkg key add`) is for packages only. So the `bmds-apps` key must not go in
`trusted.pub`: whoever holds it could then sign a firmware update.

### 3.2 A third kind of built-in key: package keys

- **`keys/packages.pub`** (new; one key a line, `keys/`'s format): keys the firmware trusts for
  packages and package indexes only, never for an update. `bmds_update_keys()` (or a sibling)
  writes them as a separate table, `UpdateKeys::packageKeys()` with their comments; `Update`
  never sees them. Accepted in both acceptance modes (they are not the dev key).
- **`Signer::Package`** (appended): "signed by bmds-apps (a built-in package key)"; JSON `signer`
  and `key list`'s `kind` `"package"` (a new value; consumers skip what they don't know).
  `Trusted` keeps meaning a key that can also sign firmware.
- **`pkg key list`** shows the kind of each: trusted, package, dev, added. `pkg key add` refuses a
  key built in as any kind (it does already for trusted and dev).
- **`keys/README.md`** gains the three kinds in a table: what each can sign, where its public half
  lives, who holds the private half.

### 3.3 The `bmds-apps` key

- **Made once** with `mkpkg keygen` (new: the same as `mkupdate keygen`, so an SDK user needs only
  `mkpkg`): the private half at `~/.config/bmds/bmds-apps.key` on the user's PC, never in either
  repository; the public half, comment `bmds-apps`, committed as `bmds-apps/keys/bmds-apps.pub`
  and appended to `bmds/keys/packages.pub`.
- **Kept safe:** the user backs the private half up (lost, nothing more can be signed with it:
  every board would need a new key added, or a firmware with the new one). It goes into GitHub as
  the Actions secret `BMDS_APPS_KEY` (the file's contents) for the release workflow (§7).
- **Published** as a release asset `key.pub` on every release, so `pkg source add` finds it
  (§4) and a fork's README can say the same thing for its own key.
- **Changing it later:** a new key is made and published beside the old; the source's
  `signed-by` lists both ids for a while (firmware default and `pkg source add` of the old key's
  index both accept); then the old one goes. Written in the README, not built now.

## 4. `pkg source` (shell interface: added, append-only)

`pkg/sources` stays the one place sources live (a `;`-separated string, checked when set); these
commands edit it and **save** it (as `pkg key add` writes `/cfg`: adding a source is meant to
last). `bmds_pkg` stays portable: `Packages::Config` gains `saveSources` (the firmware's sets and
saves `pkg/sources` through `SysConf`; the host tests' keep a string).

| Command | What |
|---|---|
| `pkg source list` | each source, numbered: URL, `signed-by` and those keys' comments, the packages its cached list holds and its index's date, or "not read yet" |
| `pkg source add URL [--key FILE\|URL] [-y]` | add a source, trusting its key if needed (below) |
| `pkg source remove URL\|N [--keep-key] [-y]` | remove a source, its cached list, and the key it pinned if nothing else uses it |
| `pkg source show URL\|N` | the packages one source offers, as `list` shows them (`pkg --source` already means one more source for a run) |

**`add`**, step by step:
1. The URL normalized (a trailing `/` and `/index.bmpi` dropped) and checked as the setting checks
   it; already configured: said, nothing done.
2. **The index fetched** into the cache as `update` would (`file:` read), its signature parsed
   but not yet trusted.
3. **Its key:** unsigned (a `file:` folder only, as today): added without `signed-by`. Signed by a
   key already in the keyring (built in or added): added with `signed-by=ID`, no question.
   Otherwise the key is fetched from `--key` (a file or URL), or else `URL/key.pub`; its id must
   be the index's signer's (otherwise refused: "the key at URL/key.pub didn't sign this index").
4. **The question** (unless `-y`):
   ```
   https://github.com/jthwho/bmds-apps/releases/latest/download
     12 packages, signed by a key this board doesn't know:
       bmds-apps  (ed25519 key 3fa1c2...; from .../key.pub)
     Anyone who controls that address can publish packages you would then install.
   Trust this key and add the source? [y/N]
   ```
   (the full 64-digit key and its id shown, so it can be compared with what the publisher's
   README says.)
5. Yes: the key added to `pkg/keys` (as `pkg key add`), the source appended to `pkg/sources`
   with `signed-by=ID`, saved; the list kept in the cache (no separate `pkg update` needed);
   "Added. Its packages: minesweeper ... (pkg install NAME)".

**`remove`**: by URL or `list`'s number; the source dropped from `pkg/sources` (saved) and its
cached list deleted. Installed packages from it stay installed (they no longer upgrade). The key
its `signed-by` named is removed too if it is an added key, no other source names it, and no
`--keep-key`: asked ("Also forget the key bmds-apps? [Y/n]"), `-y` yes. Built-in keys are never
removed.

**`--json`:** `source` records (already written by `update`) gain `index`, `signedBy`, `keyName`,
`packages`, `fetched`, `error` (append-only); `add` writes a `key` record of the key in question
before asking, and with `--json` never asks (it needs `-y`, as `install`); `done` at the end.

**Host tests** (`tests/host/packages.cpp`, the fake web): add of a source signed by a built-in
key, by a known added key, by an unknown key fetched from `key.pub` (yes, no, `-y`), with `--key`
FILE and URL, a `key.pub` that isn't the signer's, no `key.pub`, an unsigned network index, a
duplicate, a `file:` folder; remove by URL and number, with and without the key going, a key
another source shares, a built-in key; `source show`; the JSON records.

## 5. The control panel: a Software group

A new group, **Software** (icon `system-software-install`, aliased in `dist/icons/aliases` to the
flat-color theme's nearest), after System:

- **Sources:** a details view (the URL; signed by; packages; last fetched or the error), and
  **Add Source...**, **Remove**, **Update Lists**.
  - **Add Source...:** a dialog with a URL field and **Look**; it runs `pkg source add --json
    --dry-run`, shows the key record as §4's question does (comment, id, the whole key, where it
    came from, the warning) and the package count, then **Trust and Add** / **Cancel** (runs it
    with `--trust ID`, §15: the key shown, or nothing). A source signed by a key already known shows "Signed by bmds-apps (a built-in
    package key)" and **Add**.
  - **Remove:** asks, with a box "Also forget its key (KEY)" when §4 would.
- **Keys:** a details view (comment, id, kind), **Add Key from File...** (a file dialog, then the
  key shown and asked), **Remove** (added keys only).
- **Options:** `pkg/dev` (Allow, Ask, Refuse), `pkg/welcome`.
- **Advanced:** `pkg/root`, `pkg/db`, `pkg/cache`, `pkg/keys` (folders).

Every `pkg/*` key gets its row (the test that every built-in key has a place, `controlpanel.md`
§5). The pages drive `pkg --json` as the welcome window does, through `PkgJob`, which moves from
`bmds_software` into a library both apps link (toolkit code, not OS code: built-in apps stay
ordinary). Board tests on scratch `pkg/*` settings, never the user's own (`board-user-state`).

## 6. The SDK as a release asset

**`bmds-sdk-X.Y.Z.tar.gz`** (`tools/release`, in `SHA256SUMS`), unpacking to `bmds-sdk-X.Y.Z/`:
- `sdk/` as it is (headers, musl's sources, libbmds, libbmdsgame, 3D, GUI, crt, `ld/`, `cmake/`,
  `tools/`);
- the host tools a package needs, moved or copied into `sdk/tools`: `mkpkg`, `bmdscontainer.py`,
  `bmdself.py`, `bmds-fault`; nothing else from `tools/`;
- `sdk/VERSION` (the release) and `sdk/TOOLCHAIN` (the `riscv32-esp-elf` GCC release it is built
  and tested with, and where to get it: ESP-IDF's `idf_tools.py install riscv32-esp-elf`, or
  Espressif's crosstool-NG release tarball, which is what CI uses).

**`sdk/cmake/BmdsPackage.cmake`** (new), included by `BmdsProgram.cmake`:
```cmake
bmds_add_package(minesweeper VERSION 1.0.0
        TITLE "Minesweeper" SUMMARY "Clear the field without touching a mine"
        CATEGORY Game LICENSE MIT HOMEPAGE https://github.com/jthwho/bmds-apps
        DESCRIPTION_FILE minesweeper/DESCRIPTION
        APPS minesweeper.app            # bmds_add_app() targets, into apps/
        PROGRAMS ...                    # bmds_add_program() targets, into bin/
        FILES share/minesweeper=...     # anything else, by its place under the root
        DEPENDS ...)
bmds_add_repository(packages KEY ${BMDS_KEY} PACKAGES minesweeper ...)
```
- `bmds_add_package()` stages the root under the build directory and runs `mkpkg make`; the
  icons default to the one app's program icons (as `bmds-dist` does), the title and category to
  its `app.desktop`. The target `NAME.bmpk`.
- `bmds_add_repository()` puts the packages in one folder and runs `mkpkg index`: the target
  writes `packages/index.bmpi` and the packages beside it, and `key.pub` (the public half of
  `KEY`, which `mkpkg keygen` writes beside the private one).
- **`BMDS_KEY`**, a cache variable: the private key file. Unset: the dev key (say so, loudly, in
  the build's output: "signed with the dev key: fine for trying, not for publishing").

`bmds-dist` keeps its recipes for this repository's own packages (it isn't part of the SDK).
`sdk/README.md` gains a "Publishing packages" section pointing at `bmds-apps` as the example.

## 7. `bmds-apps`

**Layout:**
```
bmds-apps/
  README.md               the walkthrough: §7's steps, written for someone making their own
  LICENSE                 MIT
  CMakeLists.txt          the SDK (FetchContent of a release, pinned by version and SHA-256;
                          -DBMDS_SDK_DIR=... for a checkout), each app, the repository
  keys/bmds-apps.pub      the public key (§3.3)
  minesweeper/
    CMakeLists.txt        bmds_add_program, bmds_add_app, bmds_add_package
    DESCRIPTION
    app.desktop
    icons/16.png 32.png   drawn by icons.py, committed
    main.c ui.c draw.c    the window, menus, drawing
    field.c field.h       the rules, no BMDS calls: tested on the PC
  tests/                  field.c's tests, built with the host's cc (ctest)
  tools/release           build, package, sign, index, gh release create
  .github/workflows/release.yml
```

**A release:** a tag `vN` (one for the repository; each app keeps its own version). The workflow
on the tag: fetches the SDK and the toolchain, configures with `BMDS_KEY` from the secret (written
to a file in the runner's temp folder, removed after), builds every package and the index, and
`gh release create` with `index.bmpi`, every `.bmpk` and `key.pub`. Every release holds every
package, unchanged ones at the same version (`pkg upgrade` leaves them).

**The source:** `https://github.com/jthwho/bmds-apps/releases/latest/download`. GitHub redirects
each name to the newest release's asset; the index's `File`s are relative names, resolved
against the index's URL as asked. No Pages site needed. Pre-releases aren't "latest", so a trial
release reaches nobody.

**`tools/release`** does the same as the workflow on the developer's PC (`--key`, `--publish`),
for whoever prefers not to keep the key in GitHub; the README explains both and why.

**The README's walkthrough:** install the toolchain; build and try on a board (`tools/bmds-run`,
or the `.app` copied to the card); make a key (`mkpkg keygen`), where to keep it, the secret;
`bmds_add_package` and versions (the ABI field, the SDK's version); release; tell users the one
line: `pkg source add https://github.com/OWNER/REPO/releases/latest/download`, and what the
question will show them (the key's id, to compare with the README's).

## 8. Minesweeper

C, the public GUI API only (`bmds/gui.h`): a window with a menu bar, a status bar and a canvas.
- **Game:** New (F2), Beginner (9x9, 10), Intermediate (16x16, 40), Expert (30x16, 99),
  Custom... (a dialog: width, height, mines), Best Times..., Exit. **Help:** How to Play, About.
- **Play:** left click reveals, right click flags, a click on a revealed number whose flags match
  it reveals its neighbours (also the middle button, or both). The first click is never a mine
  and opens space (the mines are placed after it, away from it and its neighbours). Keys: arrows
  move a cursor, Space reveals, F flags, so it plays without a pointer. Empty regions open by a
  flood fill with an explicit stack (no recursion on a small stack).
- **Status:** mines left, the time (the toolkit's `Timer`, once a second, only while playing),
  won or lost; on a loss every mine shown and wrong flags marked.
- **Best times** per level in the app's own data folder (`apps.md` §6).
- **Art drawn in code:** raised and sunken cells, numbers in eight colours, a flag, a mine, all
  scaled to the cell size (16 pixels, 24 on large screens); the window sized to the board. Its own
  look, not a copy of anyone's. Icons drawn by `icons.py`.
- **Tests:** `field.c`'s on the PC (placement keeps the first click clear, counts, the flood fill,
  chording, win and loss); on the board, a game won and lost at each level, and screenshots.

## 9. BMDS's defaults

`pkg/sources`: `https://jthwho.github.io/bmds/packages;https://github.com/jthwho/bmds-apps/releases/latest/download signed-by=ID;file:/sd0/p0/packages`,
the id that of `keys/packages.pub`'s `bmds-apps` key. A board that saved its own `pkg/sources`
keeps it; `pkg source add` adds the line for it.

## 10. Steps

1. **Keys:** `keys/packages.pub` and its table, `Signer::Package`, `pkg key list`'s kinds,
   `mkpkg keygen`, `keys/README.md`. The `bmds-apps` key made (the user keeps the private half).
2. **`pkg source`:** list, add, remove, show, `Config::saveSources`, JSON; host tests;
   on the board with a scratch `pkg/*`, against a `file:` folder and an `http:` server on the PC.
3. **The control panel's Software group;** `PkgJob` shared. Board tests and screenshots.
4. **The SDK:** `BmdsPackage.cmake`, the tools in `sdk/tools`, `sdk/VERSION` and `TOOLCHAIN`,
   the tarball in `tools/release`, `sdk/README.md`. Checked by building `bmds-apps` from the
   tarball made by a `--dev` release.
5. **`bmds-apps` and Minesweeper**, built against this checkout (`BMDS_SDK_DIR`), installed on the
   board from a `file:` source and from the PC's `http:` server through `pkg source add`.
6. **GitHub (the user's go-ahead):** `gh repo create jthwho/bmds-apps`, the secret, the workflow;
   a pre-release first (not "latest", so tried by its own download URL); then the first release,
   and BMDS's default source (§9). The workflow's SDK fetch needs a BMDS release that holds the
   tarball: until then it builds against a pinned BMDS commit's `sdk/`.

## 11. Budget

Firmware: `pkg source` and the package keys' table, a few KB of flash, nothing resident. The
control panel's pages are in its program on the card. The SDK tarball: about the size of `sdk/`
(mostly musl's sources).

## 12. Open

- Moving the non-core apps (`blocks`, `doom`, `angel`, ...) to `bmds-apps`: a later plan.
- The store (`packages.md` §18) will show which source a package comes from, and add sources
  through the same `pkg source add --json`.

## 13. As built: step 1, keys (2026-11-15)

- **The build:** `bmds_update_keys()` also reads `keys/packages.pub` into `BMDS_PACKAGE_KEYS`,
  `BMDS_PACKAGE_KEY_COUNT` and `BMDS_PACKAGE_KEY_COMMENTS`, a table apart from the update keys
  (the dev key refused there). `UpdateKeys::packageKeys()` and `packageComment()`; `accepted()`,
  which `Update` checks against, is unchanged.
- **`bmds_pkg`:** `Config::packageKeys` and `packageKeyComment`; the keyring lists them after the
  update keys as `Signer::Package` ("a built-in package key"; `package` in the status file, the
  JSON and `pkg key list`). Built in, so `key add` refuses them ("in the keyring already (built
  in)") and `key remove` too; `pkg/dev refuse` doesn't touch them.
- **Tools:** `mkpkg keygen KEY [--pub PUB] --comment TEXT` (the public half beside the private
  one by default; prints the id and the `signed-by=` it makes). `mkpkg make` and `index` say so
  when they sign with the dev key for want of `--key`; `bmds-dist` and `tools/release` name the
  dev key explicitly (BMDS's own packages use it on purpose). `mkpkg info` checks against
  `packages.pub` too; `mkupdate info` never does.
- **The `bmds-apps` key:** made with `mkpkg keygen`, comment `bmds-apps`, id
  **`37ddce8c8eb0890a`**; the private half at `~/.config/bmds/bmds-apps.key` (mode 600, folder
  700) on the user's PC, to be backed up; the public half in `keys/packages.pub`.
- **`keys/README.md`** rewritten around the four kinds (a table: where each public half lives,
  who holds the private half, what it can sign), publishing your own packages (keygen, keeping
  the private half, `key.pub`, the one line a user types), changing keys, and why a repository's
  key never goes in `trusted.pub`.
- **Tests:** host, "a built-in package key": listed (text and JSON), a package it signed planned,
  installed and named as one (`info`, JSON `signer`), refused by `key add` and `key remove`,
  installed under `pkg/dev refuse`. All 13 host suites pass.
- **On the board** (scratch `pkg/root`, `db`, `cache`, reverted after): `pkg key list` shows
  `37ddce8c8eb0890a  package  bmds-apps`; a package signed with it installed "signed by bmds-apps
  (a built-in package key)"; `key remove 37dd` refused; a `file:` source with
  `signed-by=37ddce8c8eb0890a` read its index (signed by `mkpkg index --key`).

## 14. As built: step 2, `pkg source` (2026-11-15)

- **`bmds_pkg`:** `Packages::sources()` (each source with its cached list's count and date),
  `lookSource()` (the index fetched or read into `CACHE/lists/ID.look`, its signer found in the
  keyring or its key read from `--key` or `key.pub` beside the index, which must be the signer's
  id and check the signature), `addSource()` (the key stored as `pkg key add` would, through
  `storeKey()` now shared with it; the source appended pinned to it and saved through
  `Config::saveSources`; the `.look` renamed into the source's list, or a `file:` folder without
  a signed index updated as `pkg update` does), `removeSource()` (by URL or number; the setting
  saved without it, its list removed, and the added keys no remaining source names handed back).
- **The command:** `pkg source list | add [-y] [--dry-run] [--key FILE|URL] URL | remove [-y]
  [--keep-key] URL|N | show URL|N`. `pkg list --source` was planned but `--source` already adds a
  source for one run, so one source's packages are `source show`. A URL is normalized (a trailing
  `/` and `/index.bmpi` dropped). JSON: `source` records (`number`, `source`, `location`,
  `signedBy` as an array, `keyName`, `listed`, `packages`, `made`); `add` writes a `key` record
  (`kind` `new` for an unknown key, with the whole key and `from`) and never asks (it needs `-y`
  or `--dry-run`); `done` records `added` and `removed`. The question:
  ```
  http://172.20.0.43:8765/repo
    1 packages, signed by a key this system doesn't know:
      Claude's test repo
      key 143a3d4ba5f172d9: ed25519 c9c1c8d3...
      from http://172.20.0.43:8765/repo/key.pub
    Anyone who controls that address can publish packages you would then install.
  Trust this key and add the source? [y/N]
  ```
- **The firmware:** `saveSources` sets and saves `pkg/sources` (`SysConf::set`, `save`). A
  source list that ends at the default value takes the line out of the store again (sysconf's
  rule), as the board test showed.
- **Not built:** the last error of each source in `list` (`update` keeps none; it says "not read
  yet (pkg update)" for a source without a list).
- **Tests:** host, two cases (152 assertions): an unknown key shown, declined (nothing changed),
  accepted (key file, setting, list kept: `pkg list` with no fetch), installed from; `list` text
  and JSON; `show` by number and URL; added twice refused; removed with its key offered and kept,
  then re-added without a question, a mirror sharing the key, removal of the last user removing
  the key; a built-in key's index added without asking; no `key.pub`; a `key.pub` of another key;
  `--key FILE`; `--json` refusing to ask and its dry run's `key` record; an unsigned network index
  refused; a `file:` folder without a signed index added unpinned; malformed and missing sources.
  All 13 host suites pass.
- **On the board** (scratch `pkg/root`, `db`, `cache`, `keys`; a repository signed with a
  throwaway key served from the PC with `python3 -m http.server`): `source list` of the defaults;
  `add` declined, then accepted (the source saved in `/cfg/sysconf`, the key added); `source
  list` and `key list` showing them; `source show 3`; `install srctest` fetched and installed;
  `remove 3` answering yes to the key; the setting back at its default and out of the store. All
  settings reverted, the scratch folders removed.
- **Size:** the firmware grew 14 KB (3,192,800 to 3,206,800 bytes), more than the few KB
  estimated: the command's text and JSON, and the look, add and remove paths.

## 15. As built: step 3, the control panel's Software page (2026-11-15)

- **The page** (`settings software`), after System: a `SoftwarePanel` (`softwarepanel.cpp`) over
  its rows, as Network's status is.
  - **Sources:** a list (source, signed by, packages; "unread" before `pkg update`), **Add
    Source...**, **Remove Source** (asks: installed packages stay; a key no other source uses
    goes too), **Update Lists** ("Read 2 sources.", or how many failed and the first error).
  - **Keys:** a list (name, id, kind: built in trusted, built in for packages, the dev key,
    added), **Add Key from File...** (the window's file dialog, through a new
    `SettingsWindow::chooseFileFor()`), **Remove Key** (added keys only; asks).
  - **Options:** `pkg/dev` (Install them, Ask first, Refuse them), `pkg/welcome`. **Advanced:**
    `pkg/sources` as a list ("as text"), `pkg/root`, `pkg/db`, `pkg/cache`, `pkg/keys`.
  - The lists are read again when a `pkg/` setting changes (a source added from a terminal too).
- **Add Source:** the address, **Look** (`pkg source add --dry-run --json`), then what it found:
  for an unknown key its name, id, the whole key, where it came from and the warning, with
  **Trust and Add**; for a known key or an unsigned folder, **Add**. Editing the address after a
  look disables Add until it is looked at again.
- **`pkg source add --trust ID`** (new): adds only if the key is still ID, and answers the
  question for it. The window's Add uses it, so a key swapped on the server between the look and
  the add is refused ("is now signed by key X, not Y: look at it again") instead of trusted
  unseen. `--json` accepts it in place of `-y`. `source add --json` also writes a `source`
  record first (`location`, `packages`, `signed`).
- **Shared:** `PkgJob` moved to `bmds_software`'s public `bmds/pkgjob.h`; `PkgRecord` reads `key`
  records (`id`, `kind`, `comment`, `key`, `from`) and `source`'s new fields (`location`,
  `keyName`, `number`, `listed`, `signed`). The icon: `system-software-install` as flat-color's
  `package`, in `rom/icons.list` (appended) and `dist/icons/aliases`.
- **Fixed in the toolkit:** `MessageBox` was sized by a wrap in pixels but drawn by a wrap in
  characters (50 columns); with the proportional font the second made more lines, the last drawn
  under the buttons (Remove Key's question). It now draws the lines it measured.
- **On the board** (scratch `pkg/root`, `db`, `cache`, `keys`; a throwaway repository served from
  the PC), each step screenshotted: the page with the default sources and the built-in keys; Add
  Source, the address typed, the unknown key shown, Trust and Add (the source saved pinned, the key
  added, both lists read again, "Source added."); Remove Source and Yes (setting back out of the
  store, the key gone); Update Lists (21 and 32 packages); Add Key from File through the file
  dialog; Remove Key and Yes. Settings reverted, scratch removed.
- **Size:** the firmware grew 11 KB (3,206,800 to 3,217,776 bytes).

## 16. As built: step 4, the SDK as a release asset (2026-11-15)

- **`tools/mksdk VERSION OUT_DIR`** writes `bmds-sdk-VERSION.tar.gz`: under `bmds-sdk-VERSION/`,
  the repository's own layout so every tool's relative paths hold (`sdk/` whole with
  `sdk/VERSION`; `tools/` with `mkpkg`, `bmdscontainer.py`, `bmdself.py`, `bmds-fault`; `keys/`
  with `dev.key`, `dev.pub`, `packages.pub`, `README.md`; `LICENSE`; a short `README.md`). Files
  git tracks, or would (a `--dev` release from an uncommitted tree); a Git LFS pointer refused.
  Sorted, the commit's time, no owner, gzip without a name or time: the same tree gives the same
  bytes (checked: two runs compared equal). 1.27 MB, 1,817 files (musl's sources most of them).
  The tools stay in `tools/` (not moved to `sdk/tools` as planned): the tarball's layout keeps
  them beside `sdk/` as here.
- **`tools/release`** makes it after the firmware, into the release's files and `SHA256SUMS`.
- **`sdk/cmake/BmdsPackage.cmake`** (included by `BmdsProgram.cmake`): `bmds_add_package(NAME
  VERSION v [TITLE] [SUMMARY] [DESCRIPTION_FILE] [CATEGORY] [LICENSE] [HOMEPAGE] [DEPENDS...]
  [APPS...] [PROGRAMS...] [FILES PLACE=SOURCE...] [ICONS...] [ABI])` stages `apps/NAME.app`,
  `bin/NAME` and the files and runs `mkpkg make` into `packages/NAME_VERSION.bmpk` (target
  `NAME.bmpk`, built by default); one application's `app.desktop` gives the title and the
  freedesktop main category, its program's `ICONS` (now a target property) the icons.
  `bmds_add_repository(NAME PACKAGES... [BASE_URL] [LABEL])` makes `NAME/` with the packages,
  `index.bmpi` (relative names unless `BASE_URL`) and `key.pub`, then checks the index with
  `mkpkg info --pub` as a board would. `BMDS_KEY` (and `BMDS_KEY_PUB`, default beside it) signs;
  unset, the dev key, with a warning at configure. Python's `cryptography` checked at configure.
- **`mkpkg make --description-file`** (a description of several lines, from a build rule).
- **`sdk/TOOLCHAIN`**: `esp-15.2.0_20250929` and where to get it. **`sdk/README.md`**: "Publishing
  packages", and the table's new rows.
- **Tried:** the tarball unpacked in a folder outside the repository; a project there (an
  application with icons and a desktop entry, a second program in `bin/`, a description file)
  built, packaged and indexed with a key made by the tarball's `mkpkg keygen`: the package's
  title ("Hi There") and category (Utility) from the desktop entry, its 16 and 32 pixel icons,
  every file checked and "signed by t.pub: SDK test"; built again: nothing to do; configured
  without `BMDS_KEY`: the dev key's warning.

## 17. As built: step 5, `bmds-apps` and Minesweeper (2026-11-15)

- **`~/src/bmds-apps`** (git initialised, nothing committed; the user commits): the layout of §7,
  with these differences: the SDK is found or downloaded by `cmake/sdk.cmake` **before
  `project()`**, since the toolchain file is in it (`file(DOWNLOAD)` with the pinned SHA-256 and
  `file(ARCHIVE_EXTRACT)` into the build folder; `-DBMDS_SDK_DIR` for a checkout), so a user runs
  plain `cmake -S . -B build`; the pin is empty until a BMDS release carries the tarball, and the
  build says to give `BMDS_SDK_DIR` until then. The rules' tests are a CMake project of their own
  in `tests/` (the PC's compiler, ASan and UBSan). The icons are drawn by `tools/icons.py`
  (standard library) and committed. `keys/bmds-apps.pub` is the public half.
- **Minesweeper** (C, `bmds/guiclasses.h` only): `field.c` (the rules: mines placed after the
  first reveal, away from it and its neighbours when there is room; an explicit-stack flood
  fill; flags; chording; the changed cells' box for partial redraws), `draw.c` (cells, numbers in
  a 5 x 7 font scaled, flags, mines, the end's crossed-out flags and the mine that went off, a
  keyboard cursor; RGB565 into the program's own picture, the changed part sent with
  `bmds_gui_put_pixels`), `main.c` (a menu bar: New F2, Beginner/Intermediate/Expert/Custom...,
  Best Times..., Quit Ctrl+Q, How to Play F1, About; a canvas that scales the board to its size;
  a status bar; a one-second timer; left reveal, right flag, both or the middle button or a click
  on a number chord; arrows, Space/Enter, F; the level, the custom size and best times in the
  app's data folder, written whole and renamed). Its own look: slate and pale cells.
- **Packaging:** `minesweeper/CMakeLists.txt` uses `bmds_add_program`, `bmds_add_app` and
  `bmds_add_package` (version 1.0.0; title, category and icons from the app); the top level's
  `bmds_add_repository(repository ...)`. The package is 116 KB (232 KB installed).
- **Releasing:** `tools/release TAG --key KEY [--sdk-dir] [--dev] [--publish [--prerelease]]`
  (a fresh `build-release/`, the files into `release/TAG/`, `gh release create --verify-tag`);
  `.github/workflows/release.yml` on a `v*` tag: Espressif's GCC from its release (the URL and
  SHA-256 `sdk/TOOLCHAIN` now gives; the asset name there was wrong and is fixed), Ninja and
  `python3-cryptography` from apt, the key from the secret `BMDS_APPS_KEY` into a mode-600 file
  removed when the step ends; a tag with `-` is a pre-release. A `--dev` release was made here
  (index, key.pub, the package).
- **`README.md`:** installing (the one line, the key's id and whole key to compare, why no
  question is asked on BMDS), the layout, building, trying on a board (a file, a PC's HTTP server,
  or the `.app` folder), how a package is made, keys (the four kinds; the private half's keeping,
  backup and secret; changing it), releasing, and making your own repository from this one.
- **Fixed in BMDS on the way:** a shown window's `width` and `height` now resize it
  (`guiprograms.md` §18): changing level couldn't fit the window before.
- **On the board** (scratch `pkg/*`; `build/repository` served from the PC): `pkg source add
  http://PC:8765/` took it with no question ("signed by bmds-apps (37ddce8c8eb0890a, a built-in
  package key)"), `pkg source show` listed it, `pkg install -y minesweeper` fetched and installed
  `apps/minesweeper.app`, `pkg info` named the signer and showed the description. The game, run
  as the launcher would (its app id and folder): a first click opening a region; a right-click
  flag; the keyboard's cursor; a loss showing every mine and the one that went off; the Game menu;
  Expert (the window grew, after the fix) and back to Beginner (it shrank); Custom... (its dialog
  remade as rows after a first try with a Form looked cramped); a 5 x 5 one-mine field won
  ("Cleared in 13 s!"); How to Play and Best Times opened and closed twice. The level was kept
  across a restart. Everything put back: the source removed, the settings reverted, the test's
  data file and folders removed.
- **Not tried:** a real win on a standard level (its best-time message): the host tests cover the
  rules, but nothing played a whole Beginner game on the board.

## 18. As built: step 6, on GitHub and a default source (2026-11-15, the user's go-ahead)

- **`jthwho/bmds-apps`** made public (`gh repo create`), its first commit pushed over SSH (the
  `gh` token has no `workflow` scope, which an HTTPS push of a workflow file needs). The secret
  `BMDS_APPS_KEY` set from `~/.config/bmds/bmds-apps.key` on `gh`'s standard input.
- **v1**, by hand (the user's choice: bmds-apps is used from the next BMDS release on, which only
  adds apps to it; nothing moves from this repository then): `tools/release --sdk-dir
  <bmds>/sdk --publish v1` from this checkout: `index.bmpi`, `key.pub`, `minesweeper_1.0.0.bmpk`.
  The workflow on the tag stopped at its new first step: no SDK pinned, a notice to release by
  hand (it builds and publishes only once `CMakeLists.txt` pins an SDK release).
  `releases/latest/download/index.bmpi` answers with v1's.
- **BMDS's default `pkg/sources`** (§9): the project's Pages list, then
  `https://github.com/jthwho/bmds-apps/releases/latest/download signed-by=37ddce8c8eb0890a`, then
  the card's folder.
- **On the board** (the new firmware; scratch `pkg/root`, `db`, `cache`, `keys`): `pkg source list`
  with the three defaults, bmds-apps' "signed by bmds-apps (37ddce8c8eb0890a, a built-in package
  key)"; `pkg update`: 21, 1 and 32 packages; `pkg install -y minesweeper` from GitHub (116,884
  bytes through the release-asset redirect), signed by the built-in package key. Reverted.
- **When BMDS releases with the SDK tarball:** pin it in bmds-apps' `CMakeLists.txt`
  (`BMDS_SDK_VERSION`, `BMDS_SDK_SHA256` from the release's `SHA256SUMS`); from then the
  workflow releases each tag itself.
