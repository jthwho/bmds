# Formatting cards from the desktop

Status: **built and seen on the board** (2026-11-02, planned with the user the same day; §9, §10);
the question on inserting a card (not at boot) still to be seen by hand. This is `packages.md` §10's "the card the board can't use says
so on the screen", and step 2 of that plan.

## 1. Goal

A card the board can't use (new and blank, zeroed, or someone else's: a Raspberry Pi's ext4)
asks once, in a notification, "Blank SD card detected: would you like to format it?" with **Yes**
and **No**. Yes opens **Format**, a window of its own, which Files can open too (File ▸ Format
Card...). Today only the log says so, and only `mkfs` formats.

The user's decisions (2026-11-02):
- The question is a **notification**, the notification mechanism grown to ask it (§2).
- **Format is a separate GUI**, started by the question and by **Files** (for a blank card, which
  has no folder in Files to start from).

## 2. Notifications that ask

Today a button is the program's alone (`NotificationServer::activate`): the system runs a
notification's target only for a click on its body, and tells the sender of a button. The kernel
(the card's service) has no loop to be told on, and shouldn't need one to ask a question.

- **`action_targets`** (appended to `bmds_notification_t`, `uint32_t`, offset 772; the size 772 to
  776; `Notification::actionTargets`, a byte): bit *i* set, button *i + 1* runs the notification's
  target, as a click on its body does. The sender is still told (`ACTIVATED`, the button's
  number). 0 (an old program's): every button is the sender's alone, as before.
- **`BMDS_NOTIFY_QUESTION`** (flag `0x4`, `Notification::Question`): answered, it's gone. A click on
  its body or a button, or its close box (no answer is also an answer), closes it, from the
  centre too: an old question isn't news. Unanswered, it stays in the centre (Do Not Disturb, or
  `notify/balloons` off) and is answered there.
- **`notify`** gains `-b LABEL` (a button that answers: it only closes a question) and `-B
  LABEL` (a button that runs the target, `-o`/`-s`/`-l`'s), up to three in order, and `-q` (a
  question). A shell script can't listen to events, so before this its notifications had no
  buttons. `notify list` is unchanged.
- **The toolkit's `Notifier`** posts a `Notification`, so it has both at once; the SDK's programs
  through `notify_send` (the flag and field checked as the others are).

## 3. The question (`bmds_storage`)

- **When:** a card is attached and nothing on it mounts. `Vfs::automount` registers its
  partitions and mounts what it can; the storage service looks at what is there:
  - **no partitions** (no table and no filesystem: new, zeroed): "Blank SD card detected";
  - **partitions, none mounted** (ext4, swap; an exFAT or FAT that wouldn't mount): "SD card
    can't be read".
  - A card with anything mounted is left alone (`packages.md` §10).
- **What:** from the kernel (owner 0), the app "Storage", icon `media-flash` (the ROM's theme has
  it: an SD card), normal urgency, no
  timeout, a question, the target `format.desktop` (Launch); the body says the card's size and
  slot, "Would you like to format it? Formatting erases everything on it." Buttons **Yes** (runs
  the target) and **No**.
- **Closed** when the card comes out, and when Format formats it (`Storage::cardFormatted()`).
  `mkfs` doesn't close it (the shell sits below the storage component); answering Yes then
  opens Format, which shows the card as formatted.
- **At boot** the notification is kept until the panel shows it (no server yet). The Welcome
  window doesn't open on a card that didn't mount (it leaves when `pkg/root` isn't there).

## 4. Format (`/bin/format`, `bmds_format`)

`format [DEVICE]` (`/dev/sd0` or `sd0`): an ordinary GUI program on the toolkit, as the Updater.
Without DEVICE, the first card nothing mounted from, else the first card.

- **The window:**
  - **Card:** a combo box of the cards (whole block devices, not the flash or the ROM).
  - **What's on it:** its size and type; "Blank: no partitions", "Partitions BMDS can't read",
    or what is mounted ("FAT32 at /sd0/p0, 2.1 GB used").
  - **A warning** when `/cfg` or the home folder is on it: "BMDS's settings and your home folder
    are on this card: formatting erases them too."
  - **Format as:** Automatic (FAT32 up to 32 GB, exFAT above: the format it names), FAT32, exFAT.
  - A status line, then **Format...** and **Close**.
- **Format...** asks once more ("Erase everything on the 32.0 GB card in sd0 and format it as
  FAT32?": Format, Cancel), then on a worker thread does what `mkfs /dev/sd0` does:
  `Vfs::eject`, `Vfs::format`, `Vfs::automount`; closes the question. The window can't be
  closed meanwhile (FatFs's `f_mkfs` has no progress: the status line says it's working).
- **Afterwards:** "Formatted: FAT32 at /sd0/p0, 31.9 GB free." `/cfg` and the home folder are
  chosen at boot (`cfg.md`), so: "Restart BMDS to start using the card: its settings and your
  home folder go onto it, then it offers software to install." with **Restart Now**. The restart
  makes the card's `cfg` from `/flash/cfg` and the Welcome window comes (nothing installed).
- **Desktop Entry** `format.desktop` (`flash/cfg/applications`, installed once into `/cfg` on
  existing systems): Name Format, `Exec=format %f`, `Icon=media-flash`, Categories System.

## 5. Files

- **File ▸ Format Card...** in every folder window: Format for the card the folder is on
  (`/sd0/...`: `/dev/sd0`), else Format choosing (and saying when no card is in a slot).
- **On a card's folder** (`/sd0`, `/sd0/p0`, one selected): **Format...** in its menu.

## 6. ABI impact

- `bmds_notification_t` grows (appended `action_targets`; the lock's size 772 to 776, noted),
  `BMDS_NOTIFY_QUESTION` (0x4) appended: `ABI:` line in the commit.
- `notify`'s options appended (`-b`, `-B`, `-q`); its output unchanged.
- `format` (a new command and window), `format.desktop`.

## 7. Budget

- The log: a byte more a notification (64 kept).
- Format: a window while it runs (PSRAM), a worker thread with an 8 KB PSRAM stack while it formats
  (as the storage service's, which mounts cards from one).
- Storage: one notification id (4 bytes).

## 8. Tests (over the serial console: the network is unplugged)

- `notify -q -B Yes -b No -l sysmon.desktop "Q" "body"`: Yes starts the System Monitor and the
  question is gone from the centre; No and the close box remove it; an old-style notification
  keeps its buttons the sender's.
- A blank card: the question at boot and on insertion; No; Yes opens Format; formatting;
  the question closed on removal. **This needs a card the user says may be erased.**
- Format from Files' File menu, from a card folder's menu, and `format` with and without DEVICE.
- Host tests: `NotificationLog` (questions closed by answers, `actionTargets` kept).

## 9. As built (2026-11-02)

- **Notifications:** `Notification::Question` and `actionTargets` in `bmds_core` (the log drops an
  answered question: `endBalloon()` Activated or Dismissed, and `activate()`, the centre's click,
  new); `notify_send` takes the flag and field; the panel runs the target for a button whose bit
  is set. The centre has no buttons: there a click on a question runs its target (Yes), and
  removing it is No.
- **Seen on the board** (`notify -q -t -1 -B Yes -b No -l sysmon.desktop ...`): Yes started the
  System Monitor and the question left the centre; No and the close box removed it, starting
  nothing; a plain `-b` button (no `-q`) ran nothing and the notification stayed, read.
- **Format** (`bmds_format`): on the user's card it said "FAT32 at /sd0/p0: 356 MB used of 32.0
  GB" and the warning that settings and the home folder are on it; Format... asked "Erase
  everything on the 32.0 GB card in sd0?", with **Cancel the default** (Enter doesn't erase a
  card: `GuiWindow::setDefaultButton()`); Enter cancelled, the card untouched. The warning first
  lost its second line: a `TextBlock` asks for its height at a guessed width, so the window's
  paragraphs are the Welcome window's `WrapText`.
- **Files:** File ▸ Format Card... in `/sd0/p0` started `format /dev/sd0`; `p0`'s menu in `/sd0`
  has Format....

## 10. The blank card on the board (2026-11-02, the user's card, backed up and restored)

- **Blanked** with `dd` (the first 4 MB zeroed: raw block devices, `storage.md` §14's first part,
  built for it). Each boot on it: no crash (the main task 3068 bytes from its stack's end), "Blank
  SD card detected: The 32.0 GB card in sd0 is blank. Would you like to format it?", the SD card's
  icon, Yes and No; `/cfg` on `/flash`; the Welcome window stayed away.
- **No** answered it (gone; the next boot asked again). **Yes** opened Format on sd0, "Blank: no
  partitions and no filesystem". Format... and Format made FAT32 at `/sd0/p0`, 32.0 GB free, and
  offered Restart Now; Restart Now restarted, the boot made the card's `cfg` from `/flash/cfg`, and
  the Welcome window installed the standard set (12 packages).
- **What looked like Format vanishing** was the test: a program the panel starts gets the serial
  console as its controlling terminal, in the foreground of its own session, so `bmdsboard.py`'s
  Ctrl-C (its sync) ended it (exit 130). Started from a shell with `&`, or from Files with no Ctrl-C
  after, it stayed. The panel's starts and the console's Ctrl-C are older than this work.
- **A restart panicked once** (of about eight): a store access fault in the ROM's
  `Cache_WriteBack_All_Gid` (MTVAL `0x3ff100a0`, the cache's registers), which `esp_restart_noos`
  calls; the next boot counted "crash 1 of 3". Not seen again; cause unknown.
- **Not seen:** the question on inserting a blank card while running (it needs hands on the card;
  the same code as at boot), and its closing when the card comes out.
