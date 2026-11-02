# BMDS release notes

Each release has a section headed `## X.Y.Z` (the version in
`components/bmds_core/include/bmds/version.h`), newest first. `tools/release` puts that section in
the update package, where the Updater shows it, and on the GitHub release; it refuses to make a
release without one.

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
