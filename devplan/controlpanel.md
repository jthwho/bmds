# Settings: the control panel

Status: **built** (steps 1-5, 2026-10-16; planned the same day with the user). Builds on
`sysconf.md` (the registry, ids, watchers, `GuiApplication::watchConfig()`), the GUI toolkit
(`gui.md`) and the System Monitor as a model of a built-in application with pages (`sysmon.md`).

## 1. Goal

- **One place to change the system graphically** (the user's): every system setting, which since
  `sysconf.md` §13 means every sysconf parameter, can be seen and changed here.
- **Grouped** (the user's): the settings for time on one page, for the network on another, and so
  on.
- **Laid out like the Mac's System Settings** (the user's): the groups down the left, each with
  its icon; the chosen group's page on the right.

## 2. Decisions (the user's, 2026-10-16)

1. **Saving: live, saved at once.** As on the Mac, a change applies and is saved to `/cfg` at
   once (`set`, then `save`). A change that could cut the user off (§6.3) asks "Keep this
   setting?" and reverts itself after 15 seconds without an answer. A value changed from the
   shell but not saved shows a mark, with Save and Revert.
2. **Pages: curated, plus All Settings.** Each group's page is laid out by hand, with its own
   labels, order, sections and special editors (a time zone picker, the display's modes, colour
   swatches, a password dialog). A last page, **All Settings**, lists every key, programs' own
   included, as a searchable tree with editors made from each key's type, limits and help, so
   nothing is out of reach.
3. **Presentation lives in the application.** The groups, labels and order are the application's
   own tables; sysconf's declarations are not changed, so the ABI isn't touched. Programs' keys
   (`sysconf_declare`) appear in All Settings under their first key component.
4. **Pages show live status.** As on the Mac: Network shows the link, the address and the DNS
   servers in use; Date & Time the clock and its sync; Display the current mode; Sound the
   devices. Read-only, from existing APIs.

## 3. Facts that shape the design (checked 2026-10-16)

- **The registry is enough for generic editors.** Each parameter has a type (`bool`, `int`,
  `string`, `color`, `enum`, `path`, `paths`, `secret`), limits (`min`/`max`, a string's
  longest), an enum's choices, a one-line help and a default, and reports default, saved and
  changed (`SysConf::Info`, the SDK's `sysconf_info`/`sysconf_get`). `set` checks every value
  and says why it refuses one. Nothing new is needed in the kernel.
- **About 140 keys:**
  - 70 built-in declarations (`BMDS_SYSCONF`), two of them families: `wm/palette/*` (23
    colours) and `terminal/colors/*` (19).
  - The network's, declared at run time with `declareKernel()`: `net/ETH/ipv4/*` per interface
    when it appears, `net/dns/*`, `net/hosts`, `net/mdns/enabled`, `net/ntp/*`, `net/ptp/*`
    (11), `net/ssh/*`.
  - Programs' own keys (`sysconf_declare`), up to 64.
- **Keys appear late:** an interface's keys are declared when the device appears; a program's
  when it runs. A watcher's mask takes keys declared after it starts (`sysconf.md` §12.4a), so a
  watch on `""` sees them.
- **Some settings don't act at once:** `display/mode`, `display/buffering` and
  `display/lanembps` are read by the next `display start`, since the running desktop owns the
  display. `tmp/size` is read at boot.
- **`system/password` takes only a SHA-512 crypt hash** (its check refuses anything else), so
  the dialog hashes the password the way `passwd` does (`ShaCrypt`).
- **Lists that stayed files** (`sysconf.md` §10): the desktop menu (`wm.conf`), the key bindings
  (`controls.md`, edited by the Controls window), the clipboard's converters and `mimeapps.list`.
  They aren't sysconf, so they are out of this plan's scope (§10).
- **Status sources that exist:**
  - `NetStatus`: interface snapshots, DHCP state, a change watch and lease renewal.
  - The clock, the zone bundle (`ZoneBundle`: zone names from its index) and the NTP and PTP
    services.
  - `VideoModes` and the display's current mode.
  - Audio devices.
- **The toolkit lacks:**
  - A scrolled view of widgets: `ScrollArea` draws its own content, and `paintTree` doesn't clip
    children to a parent. A page is taller than the window at 720p.
  - A form layout: labels in a column, editors beside them, section headings, help under a row.
  - A number field, a colour swatch and a colour dialog, and an ordered-list editor (`paths`,
    `panel/items`, `net/hosts`, the DNS servers).
  - The rest is there: `CheckBox`, `ComboBox`, `Slider`, `LineEdit`, `Button`, `Label`,
    `FileDialog`, `MessageBox`, `DetailsView`/`TreeView` with icons, `Splitter`, `StatusBar`.

## 4. The window

```
+-----------------------------------------------------------------------+
| Settings                                                       _ [] X |
+------------------+----------------------------------------------------+
| [ Search       ] |  Date & Time                                       |
|                  |                                                    |
|  (i) General     |  Thursday, October 16, 2026  3:42:07 PM  PDT       |
|  (A) Appearance  |  Synchronized with pool.ntp.org 12 s ago (+1.2 ms) |
|  [=] Desktop     |                                                    |
|  [D] Display     |  Time zone                                         |
|  (S) Sound       |      Zone        [ America/Los_Angeles      v ]    |
|  (!) Notifications|                                                   |
|  (T) Date & Time |  Network time                                      |
|  (N) Network     |      [x] Set the clock from NTP servers            |
|  (H) Sharing     |      Servers     [ pool.ntp.org             ]      |
|  (K) Keyboard &  |                                                    |
|      Gamepads    |  Panel clock                                       |
|  (>) Terminal    |      Format      [ %l:%M %p                 ]      |
|  (F) Files       |                  How the time is written (strftime)|
|  (*) System      |      ...                                           |
|  ---             |                                                    |
|  (#) All Settings|                                                    |
+------------------+----------------------------------------------------+
```

- **The sidebar:** a search field, then the groups (a 16-pixel icon and a name each), with All
  Settings set apart at the end. A group whose keys are all missing (a component left out of the
  build) isn't listed.
- **The page:** the group's title, its status (§7) where it has one, then its sections, in a
  scrolled view. Each row is a label, an editor and, in grey under it, the help where the label
  alone isn't clear.
- **A row's menu** (right click): Reset to Default, Revert (to the saved value), Copy Key, Show in
  All Settings.
- **The status bar:** the last error, or "Saved".
- **Size:** about 760 × 520 to start; it can be resized, with the sidebar on a `Splitter`.
- **Keyboard** (`controls.md`): Ctrl+F searches; Up and Down in the sidebar change the page;
  Tab moves through a page's editors.
- **One window:** a second `settings` raises the first and switches its page, as a second
  `sysmon` does.

## 5. Groups

Each group's keys and sections. A row's label is the application's; help comes from the key's
declaration unless the page gives better.

| Group | Icon | Sections and keys |
|---|---|---|
| General | `preferences-system` | About (status: version, serial, uptime, memory, card); Name: `system/hostname`; Password: `system/password` (Set Password..., Remove); Home: `home` |
| Appearance | `preferences-desktop-theme` | Colours: `wm/palette/*` as swatches by role (desktop, frames, controls, selection, text), with a preview; Icons: `panel/icon-theme` (the themes found) |
| Desktop & Panel | `preferences-desktop` | Panel: `panel/edge`, `panel/items` (an ordered list), `panel/spacer/width`, `panel/windows/max-width`; Tray: `panel/tray/*`; Apps menu: `panel/launcher/*`; Desktop: `panel/desktop/menu` |
| Display | `display` | Status: mode, refresh, link rate; Mode: `display/mode` (the modes as a list, plus Custom), `display/buffering`; Full screen: `display/fullscreen-modes`; Advanced: `display/lanembps`; Screenshots: `screenshot/path` |
| Sound | `audio-volume-high` | Status: devices; Output: `audio/default`, `audio/dsp0/volume` (a slider), `audio/dsp0/mute`; Alerts: `audio/bell`, `audio/feedback`; Synthesizer: `synth/*`; MIDI: `midi/*`; USB device: `usb/device/*` (applied, not saved: `NotSaved`); Speech: `speech/*`; Advanced: `audio/dsp0/rate`, `audio/dsp0/period` |
| Notifications | `preferences-desktop-notification` | `notify/balloons`, `notify/quiet`, `notify/sound`, `notify/timeout` |
| Date & Time | `clock` | Status: the time, the zone, NTP's and PTP's state; Time zone: `clock/zone` (§6.4); Network time: `net/ntp/*`; Panel clock: `panel/clock/*`; Precision time: `net/ptp/*`; Advanced: `clock/tzdata`, `clock/leapseconds` |
| Network | `network-wired` | Status per interface: link, speed, address, gateway, DHCP's lease (Renew Lease); per interface: `net/ETH/ipv4/*`; DNS: `net/dns/servers`, `net/dns/search`; Hosts: `net/hosts` (a list); Local name: `net/mdns/enabled` |
| Sharing | `preferences-system-network` | SSH: `net/ssh/*`, with the address to connect to (`ssh root@NAME.local`) |
| Keyboard & Gamepads | `input-gaming` | Gamepads: `input/gamepads`, the pads plugged in; Bindings: Edit Bindings... (runs the Controls window, `wm/controls`) |
| Terminal | `utilities-terminal` | Colours: `terminal/colors/*` as swatches with a sample of text; Clipboard: `terminal/clipboard/*`; Shell: `shell/path` |
| Files | `folder` | `files/default-view`, `files/show-hidden`; Applications: `apps/path` |
| System | `applications-system` | Logging: `log/*`; Memory: `tmp/size`, `clipboard/limit`, `image/decode-max`; Monitoring: `sysmon/*`; Updates: `update/dir` |
| All Settings | `preferences-other` | Every key (§6.5) |

- **Every built-in key has a place:** a test (host) lists the registry's keys and fails for one
  that no page names, so a new setting comes with its row. Programs' keys are exempt.
- **Icons:** freedesktop names, built into the program (`BMDS_PROGRAM_ICONS`) like the System
  Monitor's.
  - The flat-color theme has `preferences-system`, `display`, `audio-volume-high`, `clock`,
    `utilities-terminal`, `folder` and `applications-system` (checked 2026-10-16).
  - The rest get lines in `dist/icons/aliases`:
    - `preferences-desktop-theme` to `picture`;
    - `preferences-desktop` to `frame`;
    - `preferences-desktop-notification` to `alarm`;
    - `network-wired` to `globe`;
    - `preferences-system-network` to `share`;
    - `preferences-other` to `list`.
  - `input-gaming` has no art in the theme; a pad is drawn for it.

## 6. Editing

### 6.1 Editors by type

| Type | Editor | When it applies |
|---|---|---|
| `bool` | check box | at once |
| `enum` | combo box, the words given labels by the page (`top` → Top) | at once |
| `int` | number field (arrows, typed digits, kept in `min`..`max`); or a slider where the page says | Enter, focus leaving, an arrow; a slider at release |
| `string` | line edit | Enter or focus leaving; Esc puts the value back |
| `path` | line edit and Choose... (`FileDialog`) | as `string` |
| `paths` | ordered list: Add..., Remove, Up, Down | each change |
| `color` | swatch; a click opens the colour dialog | live while the dialog is open; saved at OK, put back at Cancel |
| `secret` | Set...; Remove when set | the dialog's OK |

- **A refused value:** the field turns red and keeps what the user typed, and the status bar
  and the field's tooltip say why (`set`'s error: "Not a valid address", "Out of range
  1-65535"). Nothing is saved.
- **Pages may replace an editor** with their own: the time zone picker, the display's modes, the
  palette's swatch grid, `panel/items`.

### 6.2 Saving

- **Each change is `set(id, value)`, then `save(id)`** (§2.1). A slider saves when it is let
  go, a colour at the dialog's OK, a text field when it is committed: `/cfg` isn't written per
  keystroke or per pixel dragged.
- **A save that fails** (`NoMedia`, the card read-only): the value stays applied, the row is
  marked changed, and the status bar says "Applied, not saved: ...". The row's menu can try
  Save again.
- **Changed but not saved** (from the shell's `sysconf set`): the row shows a dot; its menu
  offers Save and Revert.
- **Takes effect later** (`display/*` but `fullscreen-modes`, `tmp/size`): the page says when
  ("At the next display start", "At the next boot") beside the row.

### 6.3 Changes that could cut the user off

- **Which:** a row the page marks Confirm: `net/ETH/ipv4/*`, `net/ssh/server` and
  `net/ssh/port` while the change would end the SSH session it is reached over, and
  `display/mode` once a live mode change exists (§10).
- **How:** the value is set but not saved, and a dialog asks "Keep this setting?" with a 15-second
  count: Keep saves it; Revert, closing the dialog or the count running out sets the old value
  back.

### 6.4 The time zone picker

- A combo box of regions (`America`, `Europe`, ...), then one of the region's cities, from the
  zone bundle's index (`clock/tzdata`); `UTC` and the `Etc/` zones under Other.
- The zone's current offset and abbreviation beside it ("UTC-7, PDT").
- `panel/clock/zone` (the panel clock's own zone, empty: the system's) uses the same picker with
  "Same as the system" first.

### 6.5 All Settings

- **A `TreeView`** by key component (`net` ▸ `ptp` ▸ `domain`), columns Key, Value, and State
  (default, saved, changed). Programs' keys are marked "(program)".
- **Under it, the chosen key's** editor (§6.1), help, type and limits, default, and saved value,
  with Reset to Default and Revert buttons.
- **The search field** filters it by key, value and help.

### 6.6 Search

- Typing in the sidebar's search field replaces the groups with the matching rows of every
  page: each a label, its group, and its key. Labels, sections, help and keys are matched,
  ignoring case.
- Choosing one opens its page, scrolls to the row and highlights it for a second.
- Esc or an empty field brings the groups back.

### 6.7 Changes from elsewhere

- **One watch on `""`** (`watchConfig()`) covers every key, those declared later included.
- On a change, the shown page updates that key's editor, unless the user is editing that field.
  A field being edited keeps its text, and its row gets a "Changed elsewhere" mark.
- An overflow reloads the page's values.
- A new interface (its keys declared) rebuilds the Network page if it is shown.

## 7. Status

| Page | Shows | From | Updated |
|---|---|---|---|
| General | Version, serial, uptime, memory used and free, the card | the system's calls | each second while shown |
| Display | Mode, refresh, DSI link rate | the display | on open |
| Sound | Devices, their rates, the default | the audio calls | on open, on a device change |
| Date & Time | The time, the zone, NTP's server, offset and last sync, PTP's state and master | the clock, the NTP and PTP services | each second while shown |
| Network | Per interface: link, speed, address, mask, gateway, DHCP's state and lease, DNS in use | `NetStatus` | on its watch |
| Sharing | The SSH server running, its address | the SSH service, `NetStatus` | on a change |
| Keyboard & Gamepads | The pads plugged in | `Pads` | on a change |

- Only the shown page's status is updated; a minimized window updates nothing.
- Where an API for a status isn't public to applications yet, the step adds a read-only call
  (§8), as an `ABI:` change brought to the user first.

## 8. Structure

- **`components/bmds_settings`**, a built-in application through the public APIs (as
  `bmds_sysmon`): `/bin/settings`, `BMDS_PROGRAM_ICONS(settings, ...)`, in the Apps menu under
  System.
  - `settingsapp.cpp`: the program, `settings [GROUP]`, the second instance raising the first.
  - `settingswindow.{h,cpp}`: the sidebar, the search and the page host.
  - `settingspage.{h,cpp}`: a page built from a table of rows. It binds editors to ids, applies
    and saves, and follows the watch.
  - `editors.{h,cpp}`: §6.1's editors.
  - `pages/*.cpp`: one per group, each a table in rodata plus its status and its own editors.
  - `allsettings.{h,cpp}`: §6.5.
- **A row in rodata:** key (or a family's prefix), label, section, flags (Confirm, Slider,
  Advanced, Later), an enum's labels, the editor to use.
- **Memory:** only the shown page's widgets exist. A page is built when it is opened and freed
  when another is, so the window costs the sidebar plus one page. Size is checked against the
  System Monitor's at the end.
- **Opened from elsewhere:**
  - The panel's tray menus gain Settings...: the network's opens Network (`network.md` §712
    planned it), the volume's opens Sound, the clock's opens Date & Time.
  - The desktop menu gains Settings....
- **The toolkit's additions** (`bmds_gui`, public, host-tested where they hold logic):
  - `ScrollView`: child widgets in a clipped, scrolled viewport. The painter is clipped to the
    viewport for the children, and the wheel scrolls it.
  - `FormLayout`: label and editor columns, section headings, help rows.
  - `NumberField`.
  - `ColorSwatch` and `ColorDialog`: hue/saturation square, value bar, hex field, the palette's
    current colours as presets.
  - `ListEditor`.

## 9. Steps

1. **The toolkit:** `ScrollView`, `FormLayout`, `NumberField`, `ListEditor`, `ColorSwatch` and
   `ColorDialog`, each tried in the widget gallery (`bmds_widgets`).
2. **The application's frame:**
   - the window, the sidebar and the search;
   - `SettingsPage` and §6.1's generic editors;
   - saving, refusals, the watch;
   - All Settings;
   - the host test that every built-in key has a page.
3. **The plain pages:** General, Appearance, Desktop & Panel, Notifications, Terminal, Files,
   System.
4. **The pages with status and their own editors:**
   - Date & Time (the zone picker);
   - Network and Sharing (the Confirm dialog);
   - Sound, Display, Keyboard & Gamepads.
5. **Ways in:** the panel's tray menus, the desktop menu, `settings GROUP`.

Each step is checked on the board: every row of the page changed and seen to act, refused
values shown, and the user's saved settings backed up first and put back after.

## 10. Not in this plan; suggestions for later

- **The lists that stayed files:** a Default Applications page (`mimeapps.list`) and an Apps
  menu editor (`wm.conf`) would make this the one place for those too. They aren't sysconf. A
  later step could add them as pages that edit the files.
- **A live display mode change:** today a new mode waits for the next `display start`. Changing
  the running desktop's mode means the window manager resizing the desktop and every maximized
  window. The Display page would then apply it at once, behind §6.3's Keep dialog.
- **Per-application settings** (`games.md`' window or full screen, a program's own keys) could
  get an Applications group listing each program with keys, once programs declare more than a
  few.

## 11. As built

### 11.1 Step 1: the toolkit (2026-10-16)

`gui.md` §4u has the detail. In short:

- **`ScrollView`** scrolls by whole rows, because a widget's rectangle can't start above the
  window. Rows above the top one are parked whole below the viewport. A row a field of which
  takes the focus is scrolled to.
- **`FormLayout`:** sections, labelled rows with wrapped help (`TextBlock`), and a shared label
  column.
- **The editors:**
  - `NumberField`, which tells a held arrow once, at its release;
  - `ListEditor`, which edits in place or adds the owner's way;
  - `ColorSwatch` and `ColorDialog`, which is live with Cancel to put back.
- **New in `Widget`:** child clipping, focus in, out and child-focused, and
  `heightForWidth()`. `LineEdit` gained focus-lost.
- **Two older faults fixed:**
  - Every `LineEdit` drew a cursor.
  - None took the focus on a click.
- **Checked:** host tests for the four pure models, and the gallery's Form tab on the board.

### 11.2 Steps 2-5: the application (2026-10-16)

- **`components/bmds_settings`**, `/bin/settings`:
  - `settings [GROUP]`; `settings -u` lists the keys no page places.
  - One window: the service `settings`, a second start sending the group.
  - `flash/cfg/applications/settings.desktop`, copied to the board's `/cfg/applications`.
  - About 37 KB of firmware.
- **The tables** (`settingstable.cpp`, pure, host-tested) hold every page's rows.
  - **Placing a key:** a row naming a key wins over a pattern matching it. A page shows a key
    only on the row that places it. The test caught `net/ * /enabled` taking `net/ntp`,
    `net/ptp` and `net/mdns`'s switches.
  - **The check §5 asked of a host test** is `settings -u` instead. The host build doesn't
    collect the built-ins, which are spread through components it doesn't build. On the board:
    0 of 139 keys on no page.
- **The editors** (`EditorSet`) are §6.1's.
  - **Kinds:** check box, combo box, number field (with its unit), slider (live while dragged,
    saved when let go), line edit, list editor, swatch.
  - **Choose...** for paths: a folder or file dialog, or a folder added to a `paths` list.
  - **Mode** (a combo box of common modes, and Custom with its field). The registry takes any
    `WIDTHxHEIGHT@HZ`; there is no list of modes to show.
  - **Time zone:** a field and Choose... A dialog of regions and places read from
    `/share/zoneinfo`, with "Same as the system" for the panel clock's. A combo box holds 32
    items, too few for a region.
  - **Password:** Set Password... asks twice in masked fields (`LineEdit::setMasked()`), then
    `Password::set()`. Remove is beside it.
- **Saving:**
  - Each change is set, then saved, with the status bar saying so, or why not. A refusal names
    the range or the type, and a refused text stays selected to be corrected.
  - Rows changed elsewhere and not saved show Save and Revert. The registry is followed through
    one watch: a row whose text is being typed isn't overwritten, and a key a page places that
    appears later (an interface) makes the page again.
  - **The Keep dialog** (§6.3): 15 seconds counted down, then put back. It applies to the
    interface's settings and the SSH server's.
- **Search** (Ctrl+F) matches labels, sections, keys and help. Each match shows with its page's
  icon. Choosing one shows the page, scrolls to the row and lights its label. Enter gives the
  setting the keys.
- **All Settings:** every key with its value and state. The chosen key's editor, facts (type,
  limits, default, saved) and Reset to Default and Revert.
- **Status** (§7), read each second while a page whose status changes is shown:
  - General: version, board, serial, uptime, memory.
  - Date & Time: the time and the zone.
  - Display: the desktop's size.
  - Network: each interface's link, address, router, lease and DNS, with Renew Lease.
  - Sharing: the `ssh` command to use, by name and address.
  - Keyboard & Gamepads: the pads plugged in, and Edit Bindings... (starts `controls`).
- **Ways in:**
  - the network icon's Network Settings...;
  - the volume control's Sound Settings...;
  - the panel menu's Date & Time Settings... and Panel Settings...;
  - the Apps menu's entry (and so the desktop's right click, which opens it).
- **The toolkit, on the way:**
  - `DetailsView::setHeaderVisible()` (the sidebar; a list editor without a title);
  - `LineEdit::setEdited()` (the search as typed) and `setMasked()`. Enter and Esc go on to
    the window when it isn't told of them, so a dialog's default button and Esc work from its
    fields.
  - `Label` and `CheckBox` `setHighlighted()`.
  - `ScrollView` hints its rows' height at its real width once it has one.
  - Five icons in the ROM theme and the flat-color aliases (`preferences-desktop-display`,
    `-notification`, `-panel`, `preferences-system-time`, `-network`, `preferences-other`).
- **Not as planned:**
  - **No row menu.** A right press reaches the deepest widget only, so rows have Save and
    Revert while changed elsewhere, and All Settings has Reset to Default.
  - **No red refused field.** The palette has no red role for a `LineEdit`.
  - **No NTP or PTP state, and no audio devices,** in the status. Their classes are private to
    `bmds_net` and the audio component; a public read-only call is the next step there.
  - **Network status is read each second** while shown, rather than on `NetStatus`'s watch.
- **Checked on the board** (the user's settings noted first and found as they were after):
  - every page shown; search for "port" and "window b";
  - a number stepped and saved, then Reset to Default in All Settings (the store back to none);
  - the Keep dialog timed out and put the MTU back;
  - the zone dialog opened at America/Los_Angeles and was cancelled;
  - the password dialog masked and was cancelled (the password untouched);
  - Edit Bindings started Controls;
  - the panel menu's Date & Time Settings... switched the running window.
- **Not tried:** setting a password, choosing a zone, the network icon's item (the same path
  as the panel menu's).

## 12. Themes, previews, rows that apply, the clocks' and audio's status (2026-10-16, the user's)

The user's requests after trying it:
- NTP's, PTP's and audio's status.
- Settings that don't apply hidden (the static address under DHCP).
- Color themes for the system and the terminal, with a preview on real-looking elements.
- Help text darker.

- **Status calls:**
  - `bmds/timesync.h` (`bmds_net`, kernel API, not the SDK's) gives NTP's and PTP's state as
    summaries: `TimeSync::ntp()` (synced, server, stratum, offset, round trip, answered when)
    and `TimeSync::ptp()` (running, port state, master, offset, path delay, who follows whom).
    PTP's full status is large, so it is filled in PSRAM.
  - Audio needed none: `AudioDevice::device()` and `status()` were public.
  - Date & Time adds an NTP line, and a PTP line while it is on. Sound shows each device: idle
    or playing, its rate, period and streams, its codec, underruns if any.
- **Rows that apply** (`Row::when`): conditions joined by `,`, each `KEY=VALUE|VALUE`,
  `KEY!=VALUE` or `KEY~ITEM` (a `;` list holding it), the row's `*` filled in.
  `SettingsTable::applies()` is pure and host-tested. 31 rows have one:
  - the interface's settings (only while it is used);
  - the address and router (only with `static`);
  - NTP's servers;
  - PTP's settings (only while it is on), its master-only ones (only if it may be master);
  - SSH's (only with the server on);
  - the CPU graph, the balloon timeout;
  - the panel items' settings (only while the item is on the panel);
  - the log files' (only with a directory).
  - **What changes:** a page hides its rows that don't apply, and a heading over none. It does
    so again at each change it is told. A search that finds a hidden row says when it applies.
- **Themes** (`themes.cpp`, pure, host-tested): a theme is every color of a set. A theme isn't
  a setting: the one shown is the one the colors match exactly, else Custom.
  - **System:** Standard (the defaults), Retro (teal desktop, grey close box), Slate, Dark,
    High contrast.
  - **Terminal:** Standard (xterm's), Tango, Solarized dark and light, Paper, Green screen.
  - **Applying one** sets every color, then saves them together with the new
    `SysConf::save(Span<const Id>)`: the store is written once, and nothing else changed but
    not saved is saved with them.
  - **The checks:** the standard themes are the defaults (`WindowPalette()`,
    `TerminalColors()`), no two themes are alike, and the system set names every role.
- **Previews:**
  - Appearance: a small desktop with an inactive and an active window (title, buttons,
    text, disabled text, a button, a field, a selection), a tooltip, the pointer and an icon's
    name.
  - Terminal: a short session (prompts, a listing, a warning and an error, the 16 colors as
    numbered blocks, the cursor).
  - Both are drawn from the registry's colors, and redrawn as any of them changes.
- **The color rows** are named, ordered and grouped:
  - Desktop, Windows, Controls, Tooltips, Pointer;
  - Text, Background and Cursor, then the 16 colors as Black to Bright white.
  - They have no help: the label and the preview say it. A pattern row after them takes a
    color added later.
- **Help text darker:** `DisabledText`'s default is now 505050 (about 4.4:1 on the face, from
  808080's 1.9:1). It is still plainly grey beside black.
- **Checked on the board** (the user's colors noted first: all default but
  `terminal/colors/color4`, saved as 0072e5):
  - Dark applied to the whole desktop at once, then Standard put every palette key back to
    default.
  - Solarized dark applied, then Standard and the user's color4 set and saved again.
  - With DHCP, Address and Router were hidden.
  - The NTP line: synchronized with pool.ntp.org, stratum 1, offset -4.55 ms.
  - Sound: dsp0 idle on the ES8311.
  - `settings -u`: 0 of 139.
- **Size:** 12.6 KB more firmware.

