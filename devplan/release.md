# Release: BMDS for Other People

Planned 2026-10-30 with the user (the user: release BMDS to the world; a web installer; GitHub as
the public home of the downloads).

## 1. Goal

Someone with an Olimex ESP32-P4-PC and no toolchain has BMDS running in a few minutes:
1. **flash the board from a web page** (§4), with nothing to install;
2. **unzip the card's files onto an SD card** (§3);
3. later releases arrive as **update packages** (`.bmup`, `update.md`) through the Updater, as
   they do now.

Someone with a toolchain builds their own, changes anything, and makes their own releases with
the same script (§5). The README (§7) explains what BMDS is, shows it, and points at both paths.

Not goals:
- **Being the official vendor of BMDS.** There is no official key. Releases accept the dev key
  (§2), so anyone can sign a package for their own boards. The trusted-key mode stays for someone
  who wants to lock their boards down.
- **Writing the card from the web page.** A zip comes first; whether the page should write the
  card too is a later discussion (§9).
- **Installing over the network.** That needs an HTTPS client, which BMDS doesn't have.

## 2. Decisions (the user, 2026-10-30)

- **Where it lives:** `git.howardlogic.com` stays the main repository, and GitHub is its public
  mirror. GitHub holds the Releases (the files) and Pages (the installer).
  *Changed 2026-11-01 (the user):* not a mirror. The repository is `jthwho/bmds`; `tools/release
  --publish` pushes each release's commit and tag there and makes the release with `gh`
  (`packages.md` §25).
- **Signing:** releases are built with `CONFIG_BMDS_UPDATE_KEYS_DEV` (the default) and signed
  with `keys/dev.key`. The signature then proves only that a package is whole and was made by
  `mkupdate`, not who made it; that suits a system people are meant to change. The README and
  `keys/README.md` say so, and how to lock boards to a key of one's own.
- **The card:** a zip of the card's files, extracted by the user onto a FAT32 or exFAT card.
  *Changed 2026-10-30:* packages instead (`packages.md`); the card zip stays only until they work.
- **Order:** the audit and the README's story first, then the installer spike (its one unknown:
  the P4 in esptool-js), with the presentation app (`preso.md`) alongside.

## 3. What a release holds

*Changed 2026-11-01:* the card zip is replaced by the packages, their index for Pages and a
packages zip for a card; see `packages.md` §25 for the list as it is now.

On the GitHub release `vX.Y.Z`, for each chip variant (§3a; `p4r1` only for now):

| File | Contents |
|---|---|
| `bmds-X.Y.Z-p4r1-flash.zip` | the flash images with their offsets (`flash.json`) and a `README.txt` with the esptool command, for browsers without Web Serial |
| `bmds-X.Y.Z-p4r1.bmup` | the update package: main firmware, ROM, recovery firmware, notes |

And once:

| File | Contents |
|---|---|
| `bmds-X.Y.Z-card.zip` | the card's files (`bmds-dist`'s rootfs: `bin`, `apps`, `share`, `etc`) |
| `bmds-X.Y.Z-sources.zip` | the upstream sources and patches of every GPL package on the card (BusyBox), which the GPL asks to be offered with the binaries |
| `SHA256SUMS` | every file's SHA-256 |
| notes | the release notes, also inside the `.bmup` |

The flash images are what `build/flasher_args.json` lists: the bootloader (0x2000), the partition
table (0x8000), the main firmware (`ota_0`, 0x10000), the recovery firmware (`ota_1`, 0x410000),
the blank `otadata` (0x570000) and the ROM (0x600000). `/flash` and `nvs` get no image: a fresh
install erases them, and the first boot formats `/flash` and fills it with its defaults.

### 3a. Chip revisions: two builds

The P4's silicon before v3.0 and from v3.0 on need different builds
(`ESP32P4_SELECTS_REV_LESS_V3`, `docs/HARDWARE.md`), and neither boots on the other. Our boards are
v1.3; Olimex's newer runs may ship v3.x. So:
- **releases are `p4r1` only for now** (the user, 2026-10-30): built for silicon before v3. The
  installer refuses v3 silicon with a clear message, and `tools/release` keeps the variant a
  parameter so `p4r3` can be added when someone with a v3 board can try it;
- **the Updater must also check the revision's top.** Today it refuses a package only when the
  chip is older than the package's least revision (`bmds_update/src/update.cpp`). A `p4r1`
  package on v3 silicon would pass, write firmware the bootloader won't start, and, with the
  recovery firmware in it too, leave a board that only USB flashing brings back. Each app image's
  header carries its least and greatest revision (`min_chip_rev_full`, `max_chip_rev_full`, bytes
  15-18): the check compares the chip with both (step 2).

## 4. The web installer

A static page in the repository's `installer/`, published on GitHub Pages. No build step: plain
HTML, CSS and JavaScript modules, with esptool-js (Apache-2.0, 0.7.0: has the P4 and its rev-1
flasher stub) vendored as its bundled module.

Why not ESP Web Tools: its manifest picks images by chip family only, not by revision (§3a). Its
install button would also be one more dependency. Our page uses esptool-js directly.

**The flow:**
1. **Connect.** The browser lists serial ports; the board is the USB-Serial/JTAG port
   (VID 0x303A, PID 0x1001). esptool-js puts it in download mode by the USB-Serial/JTAG sequence.
   Pages without Web Serial (Firefox, Safari) say so, and link the flash zip.
2. **Identify.** The chip, its revision, the flash size (16 MB expected). The page picks
   the variant for it, or refuses with a clear message (v3 silicon, for now).
3. **Choose.**
   - *Fresh install:* erases the whole flash, then writes every image.
   - *Reinstall:* writes every image but leaves `/flash` and `nvs` alone, so settings and the
     user's files there survive.
4. **Flash** with a progress bar, then verify (esptool-js's MD5 per region) and reset.
5. **Set up (optional).** Still connected, now to the console: the page waits for the shell's
   prompt, then offers the hostname, a password (password logins over SSH need one), the time zone
   and the keyboard layout, and types the `sysconf` commands for them. It shows what it types.
6. **The card.** A link to the card zip and how to extract it. A console in the browser for
   whoever wants one.

**Where the images come from:** GitHub's Release downloads have no cross-origin headers, so the
page can't fetch them. A workflow on the mirror (`.github/workflows/pages.yml`, run when a release
is published, or by hand) downloads the release's flash zips, unpacks them into the site
(`firmware/X.Y.Z/p4r1/...`), writes `firmware/latest.json`, and deploys the site. Pages' limits
(1 GB a site, 100 MB a file) leave room for a few releases.

## 5. `tools/release`

One script, run on a clean tree at a tag (refuses otherwise):
1. builds the main firmware, the recovery firmware and the ROM for each variant (`build-r1`,
   `build-recovery-r1`; `-r3` later), all through the `build` wrapper;
2. builds the card's rootfs (`bmds-dist build`) and zips it; collects the GPL sources;
3. makes each variant's flash zip from its `flasher_args.json`, and its `.bmup` (`mkupdate make`
   with the dev key and `--partitions partitions.csv`);
4. writes `SHA256SUMS` and the notes (from `RELEASE_NOTES.md`'s section for the version);
5. with `--publish`, pushes the commit and the tag to GitHub and makes the release there with `gh
   release create`, uploading everything (`packages.md` §25). Without it, leaves it all in
   `release/X.Y.Z/` to try first.

The firmware's version comes from `bmds/version.h` and the commit (`cmake/version.cmake`), as now.
IDF is pinned (v6.1-dev, `ff97953b32a`, unmodified): `docs/BUILDING.md` names the commit, so
others build what we build.

## 6. The audit

Before the first push to GitHub:
- **History:** search every commit for private keys other than `keys/dev.key`, tokens, passwords,
  the ES8311 datasheet and anything else under NDA, personal paths and addresses. If anything is
  found, the mirror gets a rewritten history (the user's choice then).
- **Licences:** every directory under `thirdparty/` and every `dist/` package listed in
  `docs/THIRD_PARTY.md` with its licence. The firmware's notices go in the ROM
  (`/rom/share/licenses`), as the card's already do (`share/licenses`). Fonts (IBM Plex: OFL;
  unscii: public domain), icons (Flat Color: MIT; Glyphs) and data (tzdata, OPL banks, Freedoom:
  BSD) included.
- **What's in the repository and shouldn't be public:** host names and addresses of the user's
  network in docs and scripts (`bmds0`, the PTP hosts) are harmless, but get a look.

### 6a. Done (2026-10-30)

**History** (252 commits, one author, 7,949 paths ever added), searched for private keys, GitHub
and cloud tokens, password hashes, SSH public keys, NDA material, PDFs and personal paths:
- **Nothing to rewrite.** The only private key ever committed is `keys/dev.key` (deliberate). The
  `$6$` hashes are the SHA-crypt spec's public test vectors (`tests/host/shacrypt.cpp`). No PDF
  or datasheet was ever committed; `devplan/audio.md` says the ES8311 guide is confidential and
  cites its section numbers instead. No SSH key but `ssh-copy-id`'s doc comment. The largest blob
  is FatFs's 1.9 MB Unicode table.
- **For the user to judge, harmless as far as I can tell:**
  - the ES8311 driver and `devplan/audio.md` cite the confidential guide's section numbers and
    use what it says (register meanings, clock limits); facts, not its text;
  - the user's LAN appears in devplans and tests: private addresses (172.20.0.0/16, 10.0.0.x),
    host names (`bmds0`, `clarke`, `sartre`), the user name `jth`;
  - `sdk/libc/patches/bmds.diff` names `/home/jth/.cache/bmds-scratch/musl-pristine` in its 28
    file headers (cosmetic; `patch -p1` ignores it).

**Licences:** every `thirdparty/` directory and `dist/` package is listed in
`docs/THIRD_PARTY.md`, all permissive but three GPL-2.0 card packages (BusyBox, Doom, the TimGM6mb
SoundFont), whose sources go with each release (§3). Changes:
- the licences of the code built into the firmware are now in the ROM, `/rom/share/licenses`
  (BMDS, ESP-IDF, FreeRTOS, Mbed TLS, newlib, musl, lwIP, littlefs, FatFs, TinyUSB, Monocypher,
  TinySoundFont, stb_truetype, stb_vorbis, dr_flac, minimp3, TJpgDec), beside the ones already
  there (IBM Plex, DMXOPL3, SVOX, Dropbear, AngelScript, the icon themes): 23 files. The build
  fails if one is missing (`components/bmds_storage/CMakeLists.txt`);
- `LICENSE` files made for the four libraries that kept theirs inside the source (stb_truetype,
  stb_vorbis, dr_flac, TJpgDec), copied from it;
- every SDK-built package now ships musl's `COPYRIGHT` with its own licence (`tools/bmds-dist`).

**Found on the way:** the card's symbolic links (BusyBox's applets) are made by running
`etc/dist/links.sh` on the board (emulated links need the FAT System attribute, which a PC's unzip
can't set). A card unzipped on a PC needs that done at first boot: §8.

## 7. The README and screenshots

- **Structure:** a hero screenshot and a paragraph; a grid of features with a screenshot each;
  the hardware; getting started (the installer, the card); building your own; honest status (what
  works, what's rough); where it's going (self-hosting); the architecture in brief; the docs.
- **Screenshots from a script.** `tools/screenshots` drives the board over SSH or the serial
  console: sets a fixed scene for each shot (wallpaper, windows at set places by `win`, programs
  started, the clock's time), takes `screenshot`, and fetches the PNGs into `docs/images/`. When
  the UI changes, it runs again. The slides deck uses the same images.
- **A photo of the board** on a desk with a monitor, for the hardware section.

### 7a. As built (2026-11-01)

- **`tools/screenshots`:** six scenes (`desktop`, `editor`, `games`, `doom`, `settings`, `music`),
  each a list of steps (start a program and wait for its window by title, place it, type into it,
  wait), driven over the serial console (`bmdsboard.py`), the PNG fetched with `scp` from
  `root@bmds0.local` into `docs/images/`. It closes only the windows it opened and ends only the
  programs it started (checked: the board left with the panel alone). It uses nothing the user
  has: `/rom/share` rather than the home folder, a game started rather than Blocks' high scores,
  Settings' Appearance page rather than General (which shows the board's serial number).
- **The README:** a hero shot and a paragraph; the features as a table of shots with a paragraph
  each, then the rest as a list; the hardware; getting started (the installer, the card, the
  first-run window, the packages zip offline); building; the status, honestly (one board and
  revision, US keyboards, 720p, `mkfs` for formatting, no memory protection, no radio); where it's
  going; the architecture in brief; the docs. The PNGs are in Git LFS (`*.png`), which GitHub
  shows in a README.
- **Found on the way:**
  - **Screenshots of a full-screen program tore** (Doom: a seam across the picture): the copy
    takes about 50 ms, Doom presents every 30, and after a swap the buffer being copied is the one
    drawn next. `Surface::holdSwaps()` now keeps a present pending while the screenshot copies (the
    scanout's interrupt checks `swapDueFromIsr()`); it waits one frame first for a swap already
    begun. Doom's shot came out whole after.
  - **`screenshot` ran out of memory** with five programs open (28.5 of 31.9 MB of PSRAM used): it
    needs the frame's copy in one block. The scenes run two or three programs.
  - **BusyBox had no `uname` or `which`** (both off in `dist/busybox/config`): on, revision 2.
    `uname -a` says `BMDS bmds0 0.1.0 ABI 1 riscv32 BMDS`: the SDK's `uname()` (`sdk/crt/linux.c`)
    now gives BMDS's version as the release (from `bmds_system_info`) and "ABI 1" as the version.
- **Still to do (§7):** a photo of the board on a desk with a monitor, for the hardware section:
  the user's to take.

## 8. First boot without a card

*Superseded (2026-10-30) by `packages.md`: the card's programs become packages that the board
installs (from the network, or from the packages zip unzipped onto the card), the board makes the
links itself, and the user is assumed to start with a card, which the board can format
(`packages.md` §10, §13). Kept for the record:*

A fresh install may boot before the card is in. The desktop runs from `/rom` and `/flash` alone,
but the card's programs are missing. The first boot without a card shows a window: no card found,
what goes on it, where the zip is. To check what happens now, then design.

The first boot with an unzipped card must also make the card's links: when `etc/dist/links.sh`
is there and its links aren't (§6a), the system runs it once, before the desktop starts.

## 9. Open questions

- **The card from the web page** (Chrome's File System Access API can write into a folder the user
  picks): later, if the zip proves awkward.
- **CI on the mirror:** host tests and a build check on every push. Builds need the pinned IDF and
  the RISC-V toolchain; GitHub's runners can do it. Not needed for the first release.
- **The programs' chip variants:** programs are built with the SDK for RV32 and don't depend on
  the silicon revision, so one card zip serves both. To confirm when `p4r3` is first tried.

## 10. Steps

1. [x] The audit (§6, §6a): history, `docs/THIRD_PARTY.md`, `/rom/share/licenses`.
2. [x] The Updater checks the chip revision's top (§3a), host-tested. Done 2026-10-30:
   `UpdatePackage::appRevisions()` and `runsOn()` (IDF's rule, the eFuse override included) on
   each app image's header, before anything is written; on the board, `update check` passed a
   real package and refused one whose image stops at v0.99 ("an app image isn't for this chip's
   revision").
3. [x] The installer spike: a page that connects, identifies and flashes the current build,
   tried by the user in Chrome. Built 2026-10-30; works for the user (2026-10-30): `installer/`
   (`index.html`, `installer.js`, `md5.js`, esptool-js 0.7.0 vendored), `tools/mkinstaller local`.
   No local server (the user's rule: none on the PC), so the page also works opened as a file:
   the tool inlines its modules through an import map of `data:` URLs (a file page can't load
   modules or fetch), and the user picks the firmware folder. Checked here: the MD5 against
   Node's on 10 cases, and the one-file page starting in headless Chrome.
   The user's tries (2026-10-30): the first connect reset the chip into download mode but never
   synced (cause unknown; the debug trace and a no-reset option added to find it); a later try
   flashed and verified. esptool-js's `after("hard_reset")` pulses RTS with DTR still high, so the
   chip went back into its ROM loader: replaced by our own reset, both lines cleared first. The
   RESET button re-enumerates the USB device, so the console finds the port again (`getPorts()`,
   the `connect` event). `/flash` came out erased, as it should: the user chose Fresh install
   (the settings are on the card, `/cfg`); the log now says which mode ran.
   Download mode is now found by itself (the user's question): one sync without a reset first
   (the ROM loader answers it, a running firmware doesn't); a reset otherwise, and if the sync
   after the reset fails, the probe again and a connect without one, which is how the user's
   second try got through.
   The restart after an install still came back in download mode now and then in Chrome (the user,
   2026-10-30). Not reproduced with pyserial on the board: from download mode with both lines
   raised, either clearing order, and the port reopened at once or after 1 s, all booted (4 + 7
   runs). So the page now checks: the console opens first and the restart goes through it (DTR
   cleared first, then an RTS pulse), and if the ROM says "waiting for download" (it does over
   USB-Serial/JTAG: `boot:0x205 (DOWNLOAD(USB/UART0/SPI))`), it restarts again, up to 3 times.
4. [x] `tools/release`, variant builds, the zips and `.bmup`s. Done 2026-10-30: builds in
   `build-release/VARIANT-recovery` and `VARIANT` from `sdkconfig.defaults` and the variant's
   settings only (the main configuration came out identical to the developer's, all 989 settings;
   the recovery's took two Kconfig defaults the developer's stale `recovery/sdkconfig` predates),
   the card in `build-release/dist`; the flash zip reuses `tools/mkinstaller`'s `flash.json`, so
   the Pages workflow can unpack it as the installer's firmware. A `--dev` run (any tree, never
   published) made all six files: flash zip 6.1 MB, `.bmup` 11.9 MB (firmware, ROM, recovery,
   notes), card zip 33.7 MB (all 2,497 files; its 279 links left to `etc/dist/links.sh`), the GPL
   sources 11.2 MB (BusyBox, Doom with `programs/doom`, the TimGM6mb SoundFont; the SDK's source is
   GitHub's archive of the tag). Releases refuse a dirty or untagged tree and a version without a
   `RELEASE_NOTES.md` section; `--publish` (`gh release create --verify-tag`) is untested until
   the mirror exists. `docs/BUILDING.md` names the IDF commit.
5. [x] The installer in full (§4), and the Pages workflow. Done 2026-11-01: on the site the page
   reads `firmware/latest.json` (`tools/release site`, with the release's page and its packages
   zip for the links), fetches every variant's `flash.json`, and the chip's revision picks one.
   After the install it offers the setup (step 5 of the flow): the name, the time zone (the
   browser's, filled in) and a password, typed into the console as `hostname NAME`, `sysconf set
   clock/zone Z && sysconf save clock/zone` and `passwd` (its two prompts answered), each followed
   by an echo of its status split by quotes so the echoed line doesn't match (as
   `tools/bmdsboard.py` does); it waits for the shell by Ctrl+C and an echo, up to a minute while
   the board starts. The keyboard layout is left out: only US exists. The card step now says
   packages (the first-run window, or the packages zip). Checked: the console's answers to the
   markers and `passwd`'s prompts on the board (passwd cancelled), the one-file page in headless
   Chrome; the site page itself only once Pages serves it (no local server).
6. [x] Packages instead of the card zip, and the first run: `packages.md` steps 3-10. Its step 2
   (formatting a card from the desktop) comes after the first release: `mkfs` does it meanwhile.
7. [x] The README and `tools/screenshots` (§7). Done 2026-11-01 but the board's photo (§7a).
8. [x] The first release. Done 2026-11-02: v0.1.0 (2ab5f654) built by `tools/release --publish`
   (19 packages, 26 assets), pushed with its tag and released on `jthwho/bmds`; the Pages
   workflow deployed the site in under a minute. Checked: every site file and the page's links
   answer, the published page loads `latest.json` and its firmware in headless Chrome, and the
   board's `pkg update` reads the 19 packages from Pages over HTTPS (its default source).
