# esptool-js (vendored)

Espressif's flasher for the browser, over Web Serial: <https://github.com/espressif/esptool-js>.
License: Apache-2.0 (`LICENSE`). The bundle includes pako 2.1.0 (MIT AND Zlib,
`pako.LICENSE`).

- **Version:** 0.7.0, the npm package's `bundle.js` unchanged, as `esptool.js`
  (`esptool-js-0.7.0.tgz`, sha256
  `9c7797cfbcac3a989ab85c52c9daaa65ffc0d538c273a055757b98bb846fdf1f`; `esptool.js` sha256
  `00ebe6fb0b202976d3e2a3c6b9a6bd438c26a81d2af6739c95ac276a64c1a84e`).
- **Why this version:** it has the ESP32-P4, and its flasher stub for silicon before v3
  (`esp32p4-rev1`), chosen by the revision it reads. The bundle is one ES module with its
  dependency (pako) and every stub inside: no other files, no network.
