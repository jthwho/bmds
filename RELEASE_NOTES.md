# BMDS release notes

Each release has a section headed `## X.Y.Z` (the version in
`components/bmds_core/include/bmds/version.h`), newest first. `tools/release` puts that section in
the update package, where the Updater shows it, and on the GitHub release; it refuses to make a
release without one.

## 0.4.0

Update from 0.3.x with the Updater (or `update fetch`, then `update apply`): one step, as the
bootloader hasn't changed. From 0.2.0, the two steps in 0.3.0's release notes still apply. Then
run `pkg update` and `pkg upgrade`: AngelScript now comes from the card (the `angel` package), and
the new BusyBox brings `wget https://`.

**Scripts:** `angel` has moved out of `/rom`, so a script whose first line is `#!/rom/bin/angel`
(as the examples were in 0.3.x) no longer starts. Change that line to `#!/bin/env angel`.

- **TLS for programs.** Programs can make encrypted connections through the system, as clients
  and as servers, with certificates checked against the system's roots. Their dates are checked
  too, so an expired certificate is now refused everywhere, `pkg` included (before, one was
  accepted). New in the SDK: `libtls` (`tls.h`, the familiar libtls calls) and `libbmdshttp`
  (`bmds/httplib.h`, HTTP and HTTPS with redirects and ranges). AngelScript's `Http` fetches
  `https://`, and `TlsSocket` and `Socket::startTls()` (for STARTTLS) are new. `tlsclient` (in
  `/rom/bin`) connects and shows the server's certificate, as `openssl s_client` does. BusyBox's
  `wget` fetches `https://` with certificates checked. For servers, the board makes its own key
  and certificate on first use (`tls cert` shows it). Settings are under `net/tls/`.
- **A ROM you can change.** `/rom` holds files you add: `rom install FILE... DEST`, `rom remove`,
  `rom discard`. Changes take effect at the next boot, and a power cut leaves the ROM as it was
  before or after a change, never half done. Updates now write only the system files that
  changed, and keep yours. If an update replaces one of your files, it is first copied to
  `/sd0/p0/rom-backup/VERSION/`. `rom compact` and `rom repack` gather free space, `rom export`
  and `rom import` copy your files out and back, and `rom` shows everything about the ROM
  (`info`, `ls -l`, `stat`, `map`, `log`, `diff`, `check`). A program that maps a 4 KB-aligned
  `/rom` file read-only reads it in place from flash, using no RAM for it. The ROM's programs
  are stored packed: 8.56 MB down to 4.86 MB. `docs/ROM.md` explains the ROM for developers.
- **Going back to an older release** by update package works: `update apply --force` with
  0.3.1's package, for instance. It's refused while `/rom` holds files you added, because the
  older system rewrites the whole ROM; `rom export` them first.
- **Sharing the screen, for programs.** A program can capture the screen (copied by DMA, with or
  without the pointer), follow what changes, and send keys and pointer movements. It is the
  ground for a VNC server, coming in the bmds-apps repository. The panel shows an icon while
  the screen is shared, and `display/output` can turn HDMI off (`off`), or off only while it is
  shared (`remote`). Windows that are dragged now move by copying rather than being redrawn.
- **For programs' windows (the GUI toolkit):** `ChatView`, for chat-style text (mIRC's colours
  and styles, clickable links, a "read up to here" marker, find, copying); tabs that scroll when
  there are many, with badge counts, attention marks and close buttons, which trees can show
  too; a line editor whose cursor can be read and set, which can hand Tab, Up and Down to the
  program (for completion and history) and tell it about pasted lines; right-click menus on
  lists; labels that report clicks; `Clipboard.changed`. The Widgets gallery (`guigallery`)
  shows them all.
- **The launcher** shows a submenu for each of freedesktop.org's main categories (Network,
  Settings, Education and Science are new) once anything installed is in it. A desktop entry
  with `X-BMDS-Terminal-Hold=true` keeps its terminal open after the program ends, saying how it
  ended (`terminal --hold`).
- **Reproducible builds:** the same commit builds the same bytes for the firmware, the ROM and
  every package, on any machine, so the release's files can be checked by rebuilding them.
- **Faster:** lists and trees fill in time proportional to their rows (2,000 rows took 11.7 s
  and now take 0.3 s).
- `head -3` works, as the short form of `head -n 3`.
- **Fixed:**
  - Some views left the two pixel columns at each side unpainted, showing the desktop through
    them (the Updater's notes, for one) until something covered them.
  - A form in a dialog left a gap below it, as tall as a line of its help text.
  - A local network connection whose data wasn't being read could use up the network's
    memory, after which new SSH logins hung.
  - A non-blocking TCP send could stop making progress.
  - Closing a multi-line text box (`TextEdit`) while its selection was the primary selection
    could leave the clipboard pointing at freed memory.

## 0.3.1

A fix release. Update from 0.3.0 with the Updater (or `update fetch`). From 0.2.0, the two steps
in 0.3.0's notes below still apply.

- **A crash when writing to the board's flash is fixed.** If the screen finished a frame just as
  something was written to the flash (a file saved in `/home` or `/flash`, an update starting),
  the system could stop with a "Cache error" and restart. It was rare in normal use, more likely
  while a program was drawing, and was in 0.2.0 and 0.3.0 too.
- **The bootloader question shows all of its text.** Its lines were cut off at the right, and the
  last one ("Without it, the update installs and the bootloader stays as it is.") didn't show.
  Dialogs now grow taller to fit what is in them.
- **Release notes read properly in the Updater:** paragraphs and list items wrap at the window's
  width, instead of breaking where the notes file breaks its lines.
- **The Updater shows its messages whole.** One longer than its window is wide, or than a line
  could hold (such as "Can't be installed: it isn't signed by a key this firmware accepts"), was
  cut off; it now wraps onto a second line.
- **Install Without at the version installed** (the second step from 0.2.0, declined) now says
  everything else is installed already and leaves Install with It a choice. Before, it showed a
  message cut short and the Install button stayed off.

## 0.3.0

**This release brings a new bootloader**, which memory protection needs. Updating takes two steps
from 0.2.0, because 0.2.0's Updater doesn't know about bootloaders:
1. Update with the Updater (or `update fetch`, then `update apply`) as usual. Everything but the
   bootloader is installed, and 0.3.0 starts on the old bootloader, with programs still
   unprotected.
2. Open the Updater again. It offers the same package "with a new bootloader": Install, then
   **Install with It**. Recovery writes the bootloader last, in well under a second. **Keep the
   power on until the desktop is back.** A power cut during that moment leaves a board that only USB
   flashing starts again (the web installer's Reinstall keeps your settings). From a shell:
   `update apply --bootloader FILE`.

Installing Without it is safe: 0.3.0 works on either bootloader, but runs programs unprotected
until the new one is in. Boards flashed from the web installer get it with everything else. Then
`pkg update` and `pkg upgrade` bring the new packages.

- **Memory protection.** Programs now run in the CPU's user mode, each reaching only its own
  memory. A program that crashes (a bad pointer, an illegal instruction, its stack overrun) ends
  with SIGSEGV, SIGILL or SIGTRAP and one line in the log, and the system carries on; before, it
  restarted the board. A bad pointer handed to the system is refused (EFAULT) rather than written
  through. Every existing program tested runs unchanged; one that touches the hardware itself can
  run as before with `sysconf set programs/protect false`. Thanks to Nik on Hackaday for the idea.
- **Other people's packages.** `pkg source add URL` adds someone's repository: it shows the key
  that signed it and asks before trusting it. Settings has a new **Software** page for sources and
  keys. The bmds-apps repository (https://github.com/jthwho/bmds-apps) is now a default source,
  with Minesweeper the first program in it.
- **`pkg` shows what it's doing** on a terminal: reading, downloading with a percentage,
  installing. A package folder on the card is indexed once, not on every update (17 s to 2 s for
  the card's packages).
- **For programs (the SDK):** the SDK is now a download on each release (`bmds-sdk-0.3.0.tar.gz`),
  with `bmds_add_package()` and `bmds_add_repository()` in CMake to build signed packages and a
  repository of your own. Programs carry a build-id; a crash's report gives a stack trace, which
  `bmds-fault` names from the program's ELF. Programs are built with `-g`.
- **What protection costs:** a system call takes about a microsecond more and a program starts
  about a millisecond slower; programs' own code runs as fast as before (CoreMark is unchanged).
- **Fixed:**
  - `pkg update` from an SSH session hung at full CPU, and new SSH logins hung behind it.
  - Listing a directory served by a program (sshfs, netfs) waited 30 s each time.
  - Rare silent watchdog resets, which were a crash inside the crash handler, now report
    properly.
  - A program whose second thread crashed could hang instead of ending.
  - A non-blocking TCP send under memory pressure failed with ENOMEM; it now gives EAGAIN.
  - Setting a shown window's width or height didn't resize it.
  - A message box could draw its last line under its buttons.

## 0.2.0

Update from 0.1.x with the Updater (or `update fetch`), or flash it from the web installer at
https://jthwho.github.io/bmds/. Then `pkg update` and `pkg upgrade` bring the new packages.

- **Preso**, presentations written in Markdown (Marp's format), joins the standard set and the
  launcher (Office). Six retro themes, transitions, speaker notes and a timer over SSH, slides
  scripted in AngelScript with animations and interactive pieces, live demos that run a program
  mid-talk, kiosk mode, and `preso export` to PNG. Example decks are in `share/preso/examples`.
- **BMDS Demo #1** (`pkg install demo1`): a demoscene-style show of the system's graphics, sound
  and speech, all drawn live to its own song. It has a talking BIOS, copper bars, plasma, a
  tunnel, fire with a shape burning in it, vector balls, solids, a textured cube and the
  bestagons. `-p PART` holds it on one part.
- **Formatting from the desktop.** A blank or unreadable card asks whether to format it; the
  Format window and Files' File > Format Card... do what `mkfs` does, with a warning when BMDS's
  settings are on the card.
- **For programs (the SDK):** libbmds3d, a small 3D library (wireframes, flat, Gouraud and
  textured solids, lights, made shapes, and `.bm3d` models made by `bmds-mesh3d` from OBJ or
  Bezier patches); `bmds/seq.h`, a sequencer that plays songs through MIDI with beat sync; palette
  helpers, pictures and speech in libbmdsgame. Synths can render ahead for programs that allow
  latency, so music keeps time beside heavy drawing.
- **Faster:** full-screen 8-bit pictures now go out at 60 fps through the PPA's colour table
  (Doom takes less of the system's time too); the FM synth costs about a quarter less
  beside a busy core.
- `grep` takes `-E`, `-G` and `-e`.
- **Fixed:** programs started from the desktop no longer take the serial console from its shell
  (Ctrl-C there ended them); a full-screen program that exited could crash the system.

## 0.1.1

A fix release. Update from 0.1.0 with the Updater (or `update fetch`), or flash it from the web
installer at https://jthwho.github.io/bmds/.

- **A blank SD card no longer stops the board from starting.** With a new or freshly formatted card,
  the first boot crashed while it set the card up, and every boot after it did the same, in the
  recovery firmware too. Boards whose card was already set up weren't affected. A board stuck
  that way: flash 0.1.1 from the web installer (Reinstall keeps its settings).
- BusyBox (revision 3) and Dropbear (revision 2) are rebuilt without compiler warnings; they work
  as before.
- The SDK's C library headers no longer warn in programs built with `-Wundef -Wsystem-headers`.

## 0.1.0

The first release of BMDS, for the Olimex ESP32-P4-PC with chip revisions before v3.0.

**Installing:** flash the board from Chrome or Edge with the web installer at
https://jthwho.github.io/bmds/, or with esptool from `bmds-0.1.0-p4r1-flash.zip` (its README.txt
has the command). Then put in an SD card (FAT32 or exFAT): a window on the desktop offers the
standard set of programs from the network. Without a network, unzip `bmds-0.1.0-packages.zip`
onto the card first.

**What's in it:**
- A desktop with windows, a panel, themes and settings; a terminal, Files, Edit (an IDE for
  AngelScript), the System Monitor, the Updater, the media player and the Settings window.
- A Bourne-style shell with job control, BusyBox's tools, and terminals on the desktop, the
  serial console and SSH.
- Ethernet with DHCP, DNS, mDNS, NTP and PTP; an SSH server with SFTP; HTTPS.
- `pkg`: signed packages with dependencies, from this project's repository or a folder on the
  card.
- Sound: MP3, FLAC, Ogg Vorbis, WAV and MIDI, an FM and a SoundFont synthesizer, USB and network
  MIDI.
- Games: Doom (with Freedoom), Blocks and Lucky Duck.
- C programs built with the SDK on a PC, and AngelScript programs written on the board.

**Known limits:** one board and one chip revision; US keyboards only; 1280x720 only; formatting a
card from the desktop is still to come (the `mkfs` command does it); programs share one address
space, so a faulty program can crash the machine.

The packages and updates are signed with the development key in the repository's `keys/`: the
signature shows a file is whole, not who made it.
