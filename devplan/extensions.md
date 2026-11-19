# Kernel extensions (plan, 2026-11-18: decisions open)

The user's idea (2026-11-18, the start of `rom.md` §12): "allow the user a way to extend the kernel
by providing what's basically an installable kernel extension", installed into the ROM, which can
now take the user's files (`rom install`, `rom.md` §12). This plan is step 6 of `rom.md` §12l,
with "files mapped through the MMU at 64 KB", which turns out to be needed only if extensions run
in place (§3). **Nothing is built yet: the decisions in §6 come first.**

## 1. What an extension would be

Code that runs as part of the kernel, in M-mode, not a program: a driver for a device BMDS doesn't
know (a sensor on I2C, a display, a USB class), a filesystem, a protocol, a shell command that
needs the kernel's insides. Today the only way to add such things is to rebuild the firmware.

## 2. What is there to build on (read 2026-11-18)

- **The loader** (`bmds_loader`): ELF images linked at 0 with their relocations, checked
  (`ProgramImage`), copied into the PSRAM window, patched, run. Packed images (a patch list,
  `programs.md` §20a) already exist. An extension's image could be the same format, its entry
  point an init function rather than `main`.
- **The kernel's own extension points**, all registered by linker sections today (built in only):
  boot stages, shell commands (`BMDS_SHELL_COMMAND`), sysconf keys (`BMDS_SYSCONF`), devices
  (`/dev` nodes), filesystems served by programs (`fs_serve`, slot 123: already a user-mode way).
- **User-mode drivers already work for some cases:** a program with `fs_serve` is a filesystem;
  the GUI services, network sockets and `/dev` nodes cover much of what a driver does, safely
  (U-mode, `protection.md`), with a program's crash ending only that program.
- **The crash counter** (`update.md` §18): three crashes in a row boot recovery. An extension that
  crashes the kernel at boot would land there.
- **Flash and the MMU:** the ROM partition is mapped once for data (`ESP_PARTITION_MMAP_DATA`). Its
  addresses aren't executable: the PMA (16 entries, all used and locked, `protection.md` §12) gives
  execute to the firmware's own code and to programs' PSRAM, not to the ROM's mapping. Running
  code in place from the ROM needs another mapping in an executable range, and a PMA entry there
  is not free. Fetching code from flash is also no faster than from PSRAM (`rom.md` §6).

## 3. The choices

**A. Kernel-mode extensions (native).** An ELF image like a program's, loaded at boot (from
`/rom/extensions/`, in a set order), relocated against a **frozen kernel extension table** (a
`bmds_kx_t` of function pointers and versions, like the programs' `bmds_sys_t`, but to the
kernel's services: register a device, a command, a filesystem, a boot stage, an interrupt
handler, timers, memory), its init called; it runs in M-mode with the kernel's rights.

- Loaded **as programs are: copied into PSRAM** (or internal RAM for code an interrupt runs), not
  run in place: no MMU mapping work, no PMA entry, the loader's checks and patching reused. So
  "files mapped through the MMU at 64 KB" isn't needed (§12d's 64 KB alignment stays, for later
  uses: a SoundFont's samples read in place, say).
- The table is an ABI like the programs': append only, frozen once shipped. That is the big cost:
  every kernel service an extension may call is promised for good.
- A crash is the kernel's: the crash counter and recovery catch a bad one at boot; recovery never
  loads extensions; `extensions/enabled` (a setting) and a key held at power-on (as Esc for
  recovery, `update.md` §18) start without them.

**B. User-mode drivers (programs).** Extend what programs can do instead: the slots a driver
needs (I2C and SPI transfers to a device the board's definition lets programs have, GPIO, an
interrupt delivered as an event), and service programs started at boot from
`/rom/extensions/` (or the `/apps` model). Safe (U-mode, a crash ends the program), no new kernel
ABI beyond the slots, but slower for anything per-interrupt, and not for everything (a filesystem
on a block device, say, would need more).

**C. Both, in order:** B first for what it covers (most devices on a bus), A later for what can't
be a program.

## 4. If A: the pieces

1. The extension table (`sdk/include/bmds/kx.h`), its lock test (as `abilock.c`), the first
   services.
2. The loader's second kind of image (an init and a fini, no `main`, no process), loaded into
   kernel memory; `devplan/programs.md`'s checks reused.
3. Loading at boot: a stage after storage, from `/rom/extensions/` (the ROM, so it's there without a
   card) in name order; each logged; one that fails to load is skipped.
4. Safety: recovery without them, the setting, the key at power-on; `kx list`, `kx load`,
   `kx unload` (if an extension can be unloaded at all: only if it frees all it registered).
5. An example extension in the SDK, built and installed with `rom install`.

## 5. Budget (estimates)

- A: the loader's second kind of image (a few KB), the table (a few hundred bytes plus whatever
  each service costs), the boot stage. B: a slot per kind of access.

## 6. Decisions (the user's)

- **D1. Native (A), user-mode (B), or both (C)?** Recommended: C, B first. Most drivers are a bus
  and a few registers, which a program can do safely; A only where it can't.
- **D2. Which extension first?** It decides the first services (asked at the start of this work,
  still open): a device, a filesystem, a protocol?
- **D3. For A: run in place from the ROM?** Recommended: no (copy into PSRAM as programs are), as
  §2 explains; then nothing maps ROM files through the MMU at 64 KB.
