# Tabs for Many Pages

Planned 2026-11-16 for the IRC client (bmds-apps' `devplan/irc.md` I7: its channels and private
chats in tabs by default, a tree instead in View). The client ships after the next BMDS release
anyway, so `Tabs` can gain what it needs in that release. **Approved as proposed** by the user
(2026-11-16): T1-T6 as §4 proposes.

Status: **steps 1 and 2 built** (§7, §8); step 3, the gallery and `guitest`, next.

## 1. Why

`Tabs` (`TabWidget`, mediaplayer.md §6d) was made for dialogs and the gallery: a few pages with
fixed titles. A chat client's tabs are another thing: dozens of them, opened and closed all the
time, each saying whether something happened in it. Today's widget:

- holds at most 16 tabs (`MaxTabs`, a fixed array);
- cuts the row off when the tabs don't fit: the tabs past the edge can't be reached by the mouse;
- has no close button, no mark for news in a tab, no count;
- adds a page only at the end, so a private chat can't go beside its network's channels;
- always shows its row, so a program that picks pages some other way (a tree, a wizard's
  buttons) can't use it as a stack of pages;
- has no right-click.

## 2. The additions (append only)

| Class | Kind | Name | Type | What |
|---|---|---|---|---|
| `Tabs` | property | `barVisible` | Bool, RW | The row shown (default) or not: the pages stacked, `current` choosing |
| `TabPage` | property | `badge` | String, RW | A few characters in a pill after the title (`"3"`); empty: none |
| `TabPage` | property | `attention` | Int, RW | 0 nothing, 1 activity, 2 alert: the title in the desktop's colour for each; the current tab's set back to 0 when the user changes to it |
| `TabPage` | property | `closable` | Bool, RW | A close button on the tab (on the current one and under the pointer); a middle click too |
| `TabPage` | property | `index` | Int, RW | Its place in the row; written, the page moves there (the others shift) |
| `TabPage` | event | `closeRequested` | On | The close button or a middle click: the program closes it (destroys the page) or not |
| `TabPage` | event | `menuRequested` | On, (Int x, Int y) | A right click on the tab, where in the window's client area: what `Menu.popup(x, y)` takes (as `Widget.contextMenu`) |

- **No limit of 16:** the tabs are kept in an array that grows (by 16, `MemSpace::Internal`, ~48
  bytes a tab), up to 256 a `Tabs`; past it `Create` of a `TabPage` is `NoMem`.
- **A row too narrow scrolls:** arrows at its right end step it a tab at a time, the wheel over the
  row scrolls it, and a list button opens a menu of every tab (its badge beside it, alert ones
  first-marked); the current tab is always scrolled into view. Ctrl+Tab and Ctrl+PageUp/Down
  still step through all of them.
- **`attention`** is the program's to set (it knows what counts as a mention); the widget only
  clears it when the user changes to the tab, so a tab a program marks while current stays as the
  program said. The colours are the desktop's own (an accent for activity, a red for an alert),
  so a program never picks colours that a dark theme makes unreadable.
- **`index`** reads a page's place, and moving it is one property write; `Create` still adds at
  the end.
- **`menuRequested` carries the point** (changed in step 2, §8): events carry several values, as
  `Widget.contextMenu(x, y)` already does, so the handler passes them to `Menu.popup(x, y)`;
  `Menu.popupAtPointer` isn't needed.

Not in it: dragging tabs to reorder them (a later `Tabs.movable` and `TabPage.moved(Int)`, if
wanted), icons on tabs, tooltips (no widget has them yet), several rows (§4 T1).

### 2a. The tree, for the IRC client's other view

The client's View menu offers a tree instead of the tabs (irc.md I7). `TreeView` would need the
same two marks, appended the same way:

| Class | Kind | Name | Type | What |
|---|---|---|---|---|
| `TreeView` | method | `setBadge` | None (String path, String badge) | As a tab's badge, after an item's text |
| `TreeView` | method | `setAttention` | None (String path, Int level) | As a tab's `attention`; cleared when the item becomes current |
| `TreeView` | event | `menuRequested` | On, (String path, Int x, Int y) | A right click on a row (or the Menu key): its path (empty: none), and where in the window's client area |

## 3. ABI

Names appended to `sdk/gui/classes.def` (the generated `bmds/guiclasses.h` and
`docs/gui-classes.md`), lines appended to `tests/scripts/gui.lock` and angel's `api.lock`. Nothing
changes for a program that uses none of them: a new tab is still added at the end, with no badge,
no attention and no close button, the row still shown. A program that needs them on an older
firmware finds the names missing in `Describe` (the IRC client checks `TabPage`'s `closable`
when it starts, beside `ChatView`).

## 4. Decisions to make

| | Proposed | Instead |
|---|---|---|
| T1 | **A row that scrolls**, with arrows and a list of every tab | Several rows when they don't fit (mIRC's switchbar: every tab in view, but the pages shrink as rows are added, and a narrow window is mostly tabs) |
| T2 | **Attention as three levels** in the desktop's colours | A colour a tab, the program's (readable only on the theme it was picked for) |
| T3 | **The program closes a tab** (`closeRequested`; it may ask first, or part the channel) | The widget closes the tab itself and tells the program after |
| T4 | **`index` RW** to place a page | `insertAt` as a method on `Tabs` (a page would be made and then moved anyway) |
| T5 | ~~`Menu.popupAtPointer`, events kept to one value~~ **`menuRequested(x, y)`** (step 2: events carry several values) | `menuRequested` with the point packed in an Int |
| T6 | **The tree's marks (§2a) in this release too**, as the client's tree view needs them | The tree view without marks until a later release |

## 5. Steps

1. **The row, portable** (`TabRow`, host-tested): placing tabs with badges and close buttons,
   scrolling, what a point hits (a tab, its close button, an arrow, the list button).
2. **`TabWidget`:** the growing array, the scrolled row, the marks, close and middle click, the
   right click, moving a page, the row hidden; the class additions, their
   locks. The tree's marks (§2a).
3. **`guigallery`'s tabs and `guitest`:** a page of tabs made and closed by buttons, with badges
   and attention set on a timer; `guitest` covers every property and its range.
4. Then the IRC client (bmds-apps) uses them.

## 6. Budget

Estimated: 3-4 KB of flash in `bmds_gui` and the service; ~48 bytes a tab in internal RAM,
allocated 16 at a time. A `Tabs` of a few pages costs what today's fixed 16 do (~768 bytes); a
chat client's 40 tabs about 2 KB.

## 7. As built: step 1, the row (2026-11-16)

`TabRow` (`bmds_gui`, pure, host-tested) gains the scrolling row beside its old `place()` and
`at()`, which `TabWidget` keeps using until step 2 moves it over (then they go):

- **`parts()`:** a tab's width and where its badge's pill and close button go. The title at `Pad`
  (8); a pill `BadgeGap` (4) past it, its text `BadgePad` (4) from each edge; a close button 12
  pixels square, 4 past what is before it and 4 from the right edge (instead of the right
  `Pad`); the width up to a column. With neither, it is `place()`'s width, so a dialog's tabs
  keep their look. A closable tab keeps its button's room whether the button is drawn or not
  (drawn on the current tab and under the pointer), so tabs don't move as the pointer does.
- **`placeAll()`:** lefts from 0 in the row's own measure, as `uint32_t` (256 wide tabs pass
  65535).
- **`bar()`:** a row that holds every tab has no buttons; one that doesn't keeps three 16-pixel
  buttons (back, forward, the list) at its right and the tabs' view the rest, down to a column.
  The tabs' widths being columns too, every scroll the functions give is a column multiple, so
  titles stay on the columns `Painter::text()` starts text on.
- **`scrollToShow()`:** the least change that shows a tab whole (its left edge, if it is wider
  than the view); `maxScroll()` bounds it, which also pulls a scroll back in after a widening.
- **`scrollBy()`:** a tab at a time (the arrows, the wheel): to the next tab's left edge, or the
  one before; the end is where the last tab fits.
- **`hit()`:** a tab, its close button, one of the buttons, or nothing; the tab found by a binary
  search, as the pointer's moves ask it often.

Tested on the host: each part's place, the edges of the view and the buttons, the close
button's first and last pixel, scrolling both ways and past the end; and 500 random rows (up to
256 tabs, any width from 1 to 1400): the scroll always a column multiple and in range, a tab
asked for wholly shown and found by a point in its middle.

## 8. As built: step 2, the widget and the classes (2026-11-17)

- **T5 changed:** the design assumed an event carries one value; they carry several
  (`Widget.contextMenu(x, y)`, `Shell.windowChanged`), so `TabPage.menuRequested` and
  `TreeView.menuRequested` carry the click's place in the window's client area, which is what
  `Menu.popup(x, y)` takes. No `Menu.popupAtPointer`.
- **Every scroll is a tab's left edge** (a change to step 1's `TabRow`): drawing a tab cut at the
  row's left would mean drawing it from a negative x, so the view always starts with a whole tab
  and only its right end is cut (by a clipped painter). `maxScroll()` is the first tab's edge
  from which the rest fit (a blank past the last tab, at most a tab wide); `scrollToShow()` goes
  to the first edge from which the tab is whole. The widget keeps the first tab shown, not a
  pixel offset, so a badge widening a tab before it doesn't leave the row starting mid-tab.
- **`TabWidget`:** its tabs in four arrays (the tabs, their lefts, widths and close buttons'
  places, as `TabRow` takes them) grown 16 at a time in internal RAM, to 256; past that a
  `TabPage`'s `Create` is `NoMem`. The badge's pill and the close button as step 1 placed them;
  the title in `Highlight` for activity and `CloseBox` for an alert (the badge's pill the same,
  grey when quiet). The close button shows on the current tab and the one under the pointer, lit
  red under it, and acts on release over it; a middle press closes at once. A right press on a
  tab (after a listened `contextMenu`, which still comes first) asks `MenuRequested`. The row
  repaints alone on hover and scrolling. The list holds the 32 tabs nearest the current one (a
  menu's most), an alert marked `*` and a badge in brackets. Without its row the pages fill it,
  flush (no frame or inset), and its size hint is theirs.
- **`TreeView`:** marks by path in a list (PSRAM), drawn after the name: a badge pill, the name in
  the attention's colour; a row the user makes current has its attention cleared (in the
  service, on `currentChanged`); `clear` forgets the marks too. `menuRequested` comes from
  `ItemView`'s menu request (a right press selects the row first, as every view's does).
- **The service:** the `Tabs` object keeps its pages' ids, so a tab's events come from its
  `TabPage`. `index` written moves the page; `attention` past 2 and `index` past the end are
  `BadValue`.
- **ABI:** appended to `classes.def`, `gui.lock` and angel's `api.lock` (16 lines, all new).
- **Size:** the firmware 4.5 KB larger (3313440 to 3318016 bytes).
- **On the board:** a 360-pixel window of 20 tabs (an angel script) scrolled by the buttons and
  the wheel, a tab chosen from the list scrolled into view, a closable tab's button, a middle
  click and a right click each told; badges and attention drawn; the gallery's 11 tabs scroll
  in its 440-pixel window. A tree with marks drew them, cleared a row's attention when it became
  current, and told a right click with its path. `guitest` fails three checks that predate
  this (`Clipboard.changed` reaching its Clipboard objects after `ChatView.copy()`, and a tray
  icon count): step 3 fixes `guitest` for them.
