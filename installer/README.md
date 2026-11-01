# The web installer

Flashes BMDS onto an Olimex ESP32-P4-PC from Chrome or Edge, over Web Serial, with nothing to
install (`devplan/release.md` §4). Plain HTML and JavaScript modules, no build step:

- `index.html`: the page;
- `installer.js`: connecting, checking the chip and flash, flashing, the console;
- `md5.js`: MD5, which esptool-js needs to verify what it wrote;
- `vendor/esptool-js/`: Espressif's flasher (Apache-2.0), unchanged.

The firmware is described by a `flash.json` (`tools/mkinstaller` documents it). On a web server
the page fetches `firmware/latest.json`, which names it (the GitHub Pages workflow, to come).

## Trying a build

```
tools/mkinstaller local
```

writes `build/installer/bmds-installer.html` (the page as one file: a page opened from a file
can't load modules or fetch, so its scripts are inlined) and `build/installer/firmware/`. Open
the HTML in Chrome or Edge, choose the firmware folder, connect, install. Close any serial
monitor on the board's port first: only one program can have it.

**Reinstall** writes every image but leaves `nvs` and `/flash` alone, so settings survive.
**Fresh install** erases the whole flash first.
