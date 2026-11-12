# BMDS — Bare Metal Do Stuff

![The BMDS desktop: a terminal, the file manager and the system monitor](docs/images/desktop.png)

▶ **[Watch the walkthrough video](https://youtu.be/4rS0EzuAcG8)**: a tour of BMDS running on the
board.

BMDS turns the [Olimex ESP32-P4-PC](https://www.olimex.com/Products/IoT/ESP32-P4/ESP32-P4-PC/open-source-hardware),
a small board with a 360 MHz dual-core RISC-V chip and no radio, into a self-contained personal
computer. Plug in an HDMI monitor, a USB keyboard and mouse, an SD card and Ethernet, and you get a
desktop with windows, a shell, a file manager, an editor for writing programs on the machine
itself, sound, networking, games, and programs installed from packages. The model is DOS, not
Linux: one user, an operating system that provides services and gets out of the way, and programs
that get most of the machine. The long-term goal is a system that compiles its own programs.

BMDS uses FreeRTOS (through ESP-IDF) as its kernel and provides everything above it: the drivers,
filesystems, window system and GUI toolkit, shell, program loader, and a stable C interface for
programs built separately from the OS.

## What it does

| | |
|---|---|
| ![Edit with an AngelScript program running beside it](docs/images/editor.png) | **Programming on the machine.** Edit is an IDE for AngelScript: write a script, press F5, and it runs in its own window with graphics, sound, files and the network. C programs are built with the SDK on a PC and run from the card. |
| ![Blocks and Lucky Duck](docs/images/games.png) | **Games.** Blocks (the falling-blocks game, with its music on the synth), and Lucky Duck, a casino of Solitaire, Blackjack, dice, Video Poker and Poker against bot ducks. Gamepads work through one Controls editor. |
| ![Doom running full screen](docs/images/doom.png) | **Doom**, full screen, with Freedoom's levels and its music played by BMDS's own synthesizer. |
| ![The Settings window's Appearance page](docs/images/settings.png) | **A desktop you can set up.** A panel with a launcher, a window list and a tray; desktop icons; themes, wallpaper patterns and IBM Plex fonts; every setting in Settings or the `sysconf` command. |
| ![The media player playing a MIDI file](docs/images/music.png) | **Sound.** A media player for MP3, FLAC, Ogg Vorbis, WAV and MIDI; an FM and a SoundFont synthesizer for MIDI; MIDI to and from a PC, with the board as a USB MIDI device or over the network (AppleMIDI); low-latency audio for programs. |

And beneath the windows:

- **A shell and Unix tools.** A Bourne-style shell with pipes, redirections, job control and
  scripts, BusyBox's tools, and terminals on the desktop, the serial port and SSH.
- **Networking.** Ethernet with DHCP, DNS, mDNS (`bmds0.local`), NTP, an SSH server with SFTP (log
  in from a PC, or mount the card with sshfs), and HTTPS for packages and updates.
- **Packages.** `pkg` installs, upgrades and removes signed packages with their dependencies, from
  the project's repository over HTTPS or from a folder on the card. A new card gets a window that
  offers the standard set.
- **Updates.** The firmware updates itself from a signed update package, fetched over the network
  or copied to the card, installed by a recovery firmware, so an interrupted update can be finished.
- **Files.** The SD card (FAT32 and exFAT, read and write), a persistent partition on the flash
  for settings, the firmware's built-in files, a RAM disk, and remote folders over SFTP; a file
  manager with copy, move, the trash, drag and drop.

## The hardware

The Olimex ESP32-P4-PC: an ESP32-P4 (two RISC-V cores at 360 MHz, 768 KB of internal RAM, 32 MB of
PSRAM), 16 MB of flash, HDMI out, four USB-A ports, 100 Mbit Ethernet, a microSD slot, an audio
codec with a 3.5 mm jack, and USB-C for power and the serial console. BMDS runs on the chip
revisions before v3.0 (ours are v1.3); boards with v3 silicon need a separate build that hasn't
been tried yet. See [`docs/HARDWARE.md`](docs/HARDWARE.md) for everything known about the board.

## Getting started

1. **Flash the board from your browser.** The web installer at
   **<https://jthwho.github.io/bmds/>** (Chrome or Edge, which have Web Serial) connects to the
   board on its USB-C port, checks the chip, and writes the latest release. Without such a browser,
   each release's `bmds-*-flash.zip` holds the images and the `esptool` command.
2. **Put in an SD card** (FAT32 or exFAT) and plug in the monitor, keyboard, mouse and Ethernet.
3. **Install the standard set** from the window that greets a new card. Without a network, unzip a
   release's `bmds-*-packages.zip` onto the card first: the window finds the packages there.

Later releases arrive through the Updater, or `update fetch` in a terminal. The files are on the
[Releases](https://github.com/jthwho/bmds/releases) page.

## Building your own

You need ESP-IDF v6.1 (the commit releases are built with is in
[`docs/BUILDING.md`](docs/BUILDING.md)) and the board on USB-C:

```bash
. ~/esp/esp-idf/export.sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash
```

The programs on the card are built by `tools/bmds-dist` (BusyBox, Dropbear, Doom, the games,
the SDK's examples), and `tools/release` makes a whole release. Releases are signed with the
development key in [`keys/`](keys/), so the signature proves a package is whole, not who made it:
anyone can build their own packages and updates for their own boards, and a firmware can be built
to accept only a key of its own ([`keys/README.md`](keys/README.md)).

## Status

BMDS is young and runs every day on the boards it was written on. What to expect:

- **Works:** everything above, on the ESP32-P4-PC with pre-v3 silicon, at 1280×720.
- **Rough or missing:** one board and one chip revision; keyboard layouts beyond US; display modes
  other than 720p; formatting a card from the desktop (the `mkfs` command does it); programs share
  one address space, with no memory protection between them (the chip's MMU can't provide it),
  so a faulty program can crash the machine, as on DOS. There is no Wi-Fi or Bluetooth: the chip
  has no radio.
- **Interfaces:** the C interface for programs, the shell's commands and the file formats are kept
  compatible from release to release ([`docs/ABI.md`](docs/ABI.md)).

## Where it's going

- **Self-hosting:** a C compiler, assembler and linker on the board, so programs are written and
  built on it.
- **A software store** on top of `pkg`, and a presentation app.
- More boards, through the board definitions in `components/bmds_board/`.

The plans, and what each step taught, are in [`devplan/`](devplan/); start with
[`devplan/roadmap.md`](devplan/roadmap.md).

## The architecture, briefly

- **Kernel:** FreeRTOS on both cores, through ESP-IDF, which also provides the low-level drivers.
- **BMDS:** the board definition; drivers for the display (MIPI-DSI to the HDMI bridge), USB host
  (TinyUSB), Ethernet (lwIP), audio and the SD card; a VFS over FAT, exFAT, littlefs, a ROM
  filesystem and tmpfs; consoles and pseudo-terminals; the shell; the window system and its GUI
  toolkit; the program loader.
- **Programs** are ELF files built with the SDK (`sdk/`: headers, musl, and libraries for games and
  the GUI) and loaded from the card; they reach the OS through numbered system calls that are only
  ever added to.
- Written in C++20 inside, with a plain C interface for programs.

## Documentation

| Document | Contents |
|---|---|
| [`docs/VISION.md`](docs/VISION.md) | Goals, non-goals, high-level roadmap |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Layering, components, open design questions |
| [`docs/ABI.md`](docs/ABI.md) | The programs' interface and how it stays compatible |
| [`docs/HARDWARE.md`](docs/HARDWARE.md) | The board and the chip |
| [`docs/platforms/esp32p4.md`](docs/platforms/esp32p4.md) | The chip's tricks and pitfalls, as found |
| [`docs/BUILDING.md`](docs/BUILDING.md) | Toolchain setup, building, flashing, testing |
| [`CODING_STANDARDS.md`](CODING_STANDARDS.md) | Coding conventions |
| [`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md) | Third-party software and licences |
| [`devplan/`](devplan/) | Roadmap and subsystem design plans |

The screenshots are taken on the board by [`tools/screenshots`](tools/screenshots).

## License

MIT — see [`LICENSE`](LICENSE). The third-party software BMDS includes or builds keeps its own
licence ([`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md)); the GPL programs' sources are published
with each release.
