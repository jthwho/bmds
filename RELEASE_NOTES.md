# BMDS release notes

Each release has a section headed `## X.Y.Z` (the version in
`components/bmds_core/include/bmds/version.h`), newest first. `tools/release` puts that section in
the update package, where the Updater shows it, and on the GitHub release; it refuses to make a
release without one.

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
