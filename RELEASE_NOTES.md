# BMDS release notes

Each release has a section headed `## X.Y.Z` (the version in
`components/bmds_core/include/bmds/version.h`), newest first. `tools/release` puts that section in
the update package, where the Updater shows it, and on the GitHub release; it refuses to make a
release without one.

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
