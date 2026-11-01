// @file      installer.js
// @copyright Jason Howard. All rights reserved.
//
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root folder for license information.
//
// The web installer (devplan/release.md §4): flashes BMDS onto an Olimex ESP32-P4-PC from the
// browser over Web Serial with esptool-js, then shows the board's console.
//
// The firmware comes from a descriptor, flash.json (written by tools/mkinstaller): the images and
// their offsets, the flash's size, and the chip revisions the build runs on. On a web server it is
// fetched (firmware/latest.json names it); opened as a file, the page can't fetch, so the user
// picks the firmware's folder.

import { ESPLoader, Transport } from "./vendor/esptool-js/esptool.js";
import { md5 } from "./md5.js";

const EspressifVid = 0x303a;

const $ = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const state = {
        firmware: null, // { descriptor, read(image) -> Promise<Uint8Array> }
        port: null,
        transport: null,
        loader: null,
        chip: null, // { description, revision, flashSize, mac }
        consoleOpen: false,
        consoleTransport: null,
        wantConsole: false, // reopen it when the board's port comes back
        bootText: "", // the console's latest text, for the restart's check
};

// ---- Output ----

function log(text, kind) {
        const line = document.createElement("div");
        line.textContent = text;
        if (kind) line.className = kind;
        $("log").appendChild(line);
        $("log").scrollTop = $("log").scrollHeight;
}

const terminal = {
        clean() {},
        writeLine(data) {
                log(data);
        },
        write(data) {
                log(data);
        },
};

function setStep(name) {
        for (const el of document.querySelectorAll("[data-step]")) el.hidden = el.dataset.step !== name;
}

function revisionText(rev) {
        return `v${Math.floor(rev / 100)}.${rev % 100}`;
}

// As the bootloader decides (UpdatePackage::runsOn()): at least min; at most max when max is set.
function runsOn(revisions, revision) {
        if (revision < revisions.min) return false;
        const maxSet = revisions.max !== 0 && revisions.max !== 0xffff;
        return !maxSet || revision <= revisions.max;
}

// ---- The firmware ----

function checkDescriptor(d) {
        if (d.format !== 1) throw new Error("flash.json: a format this page doesn't read");
        if (d.chip !== "ESP32-P4") throw new Error(`flash.json: for ${d.chip}, not the ESP32-P4`);
        if (!Array.isArray(d.images) || d.images.length === 0) throw new Error("flash.json: no images");
        return d;
}

function showFirmware() {
        const d = state.firmware.descriptor;
        const bytes = d.images.reduce((n, im) => n + im.size, 0);
        $("firmware-info").textContent =
                `${d.name} ${d.version} (${d.variant}): ${d.images.length} images, ` +
                `${(bytes / 1048576).toFixed(1)} MB, for chip revisions ${revisionText(d.chipRevision.min)}` +
                ` to ${revisionText(d.chipRevision.max)}`;
        $("connect").disabled = false;
}

async function fetchFirmware() {
        const latest = await (await fetch("firmware/latest.json")).json();
        const base = new URL(latest.variants.p4r1, location.href);
        const descriptor = checkDescriptor(await (await fetch(base)).json());
        state.firmware = {
                descriptor,
                async read(image) {
                        const response = await fetch(new URL(image.file, base));
                        if (!response.ok) throw new Error(`${image.file}: ${response.status}`);
                        return new Uint8Array(await response.arrayBuffer());
                },
        };
        showFirmware();
}

async function pickFolder(files) {
        const byName = new Map();
        for (const f of files) byName.set(f.name, f);
        const json = byName.get("flash.json");
        if (!json) throw new Error("That folder has no flash.json (tools/mkinstaller writes one).");
        const descriptor = checkDescriptor(JSON.parse(await json.text()));
        for (const im of descriptor.images) {
                if (!byName.has(im.file)) throw new Error(`The folder is missing ${im.file}.`);
        }
        state.firmware = {
                descriptor,
                async read(image) {
                        return new Uint8Array(await byName.get(image.file).arrayBuffer());
                },
        };
        showFirmware();
}

// ---- The board ----

async function connect() {
        $("connect").disabled = true;
        await closeConsole(); // the flasher needs the port to itself
        try {
                state.port = await navigator.serial.requestPort({ filters: [{ usbVendorId: EspressifVid }] });
                const debug = $("debug").checked;
                log("Looking for the board's download mode...");
                let description;
                if (await inDownloadMode(state.port, debug)) {
                        log("The board is in its download mode already: connecting.");
                        description = await startLoader("no_reset", debug);
                } else {
                        log("Not in download mode: the board restarts into it.");
                        try {
                                description = await startLoader("default_reset", debug);
                        } catch (e) {
                                // The reset usually gets the chip into its ROM loader even when the
                                // sync after it fails (seen in Chrome on Linux): try it as it is.
                                log(`That didn't connect (${e.message ?? e}); trying the board as it is.`);
                                await closeTransport();
                                if (!(await inDownloadMode(state.port, debug))) throw e;
                                log("The board is in its download mode now: connecting.");
                                description = await startLoader("no_reset", debug);
                        }
                }
                const loader = state.loader;
                const revision = await loader.chip.getChipRevision(loader);
                const flashSize = await loader.detectFlashSize();
                let mac = "";
                try {
                        mac = await loader.chip.readMac(loader);
                } catch {
                        mac = "";
                }
                state.chip = { description, revision, flashSize, mac };
                $("board-info").textContent =
                        `${description}, ${flashSize ?? "unknown"} flash` + (mac ? `, MAC ${mac}` : "");
                const d = state.firmware.descriptor;
                const problems = [];
                if (!runsOn(d.chipRevision, revision)) {
                        problems.push(
                                `This board's chip is revision ${revisionText(revision)}; this BMDS is built for ` +
                                        `${revisionText(d.chipRevision.min)} to ${revisionText(d.chipRevision.max)}. ` +
                                        "Builds for revision 3 and later aren't published yet.");
                }
                if (flashSize !== d.flashSize) {
                        problems.push(`This board has ${flashSize ?? "an unknown size of"} flash; BMDS needs ${d.flashSize}.`);
                }
                if (problems.length) {
                        for (const p of problems) log(p, "error");
                        $("board-problem").textContent = problems.join(" ");
                        setStep("problem");
                        return;
                }
                setStep("install");
        } catch (e) {
                log(`Couldn't connect: ${e.message ?? e}`, "error");
                if (state.transport?.traceLog) {
                        log("The serial trace (every byte sent and received):");
                        log(state.transport.traceLog.slice(-20000));
                }
                $("connect").disabled = false;
                await closeTransport();
        }
}

// A loader on a new transport, connected and running the flasher stub. @return The chip's description.
async function startLoader(mode, debug) {
        state.transport = new Transport(state.port, debug);
        state.loader = new ESPLoader({
                transport: state.transport,
                baudrate: 115200,
                terminal,
                debugLogging: debug,
                enableTracing: debug,
        });
        return state.loader.main(mode);
}

// Whether the chip is in its ROM loader already (left there by a failed connect, or by BOOT held
// while RESET is pressed): the loader answers one sync, a running firmware doesn't. Then the
// connect goes without a reset. A running BMDS gets the sync's bytes as typing, which the reset
// that follows throws away.
async function inDownloadMode(port, debug) {
        const transport = new Transport(port, debug);
        const quiet = { clean() {}, writeLine() {}, write() {} };
        const loader = new ESPLoader({ transport, baudrate: 115200, terminal: debug ? terminal : quiet });
        try {
                await loader.connect("no_reset", 1, false);
                return true;
        } catch {
                return false;
        } finally {
                try {
                        await transport.disconnect();
                } catch {
                        // already closed
                }
        }
}

async function closeTransport() {
        if (state.transport) {
                try {
                        await state.transport.disconnect();
                } catch {
                        // already closed
                }
        }
        state.transport = null;
        state.loader = null;
        return;
}

async function install() {
        const fresh = $("mode-fresh").checked;
        if (fresh && !confirm("A fresh install erases everything on the board's flash, settings and /flash included. Go ahead?")) {
                return;
        }
        $("install").disabled = true;
        log(fresh ? "Fresh install: the whole flash is erased first."
                  : "Reinstall: nvs and /flash are left as they are.");
        setStep("flashing");
        const d = state.firmware.descriptor;
        try {
                log("Reading the images...");
                const fileArray = [];
                for (const im of d.images) {
                        const data = await state.firmware.read(im);
                        if (data.length !== im.size) throw new Error(`${im.file}: ${data.length} bytes, expected ${im.size}`);
                        fileArray.push({ data, address: im.offset });
                }
                const total = fileArray.reduce((n, f) => n + f.data.length, 0);
                const before = fileArray.map((_, i) => fileArray.slice(0, i).reduce((n, f) => n + f.data.length, 0));
                if (fresh) log("Erasing the whole flash (this takes a while)...");
                const started = performance.now();
                await state.loader.writeFlash({
                        fileArray,
                        flashMode: "keep",
                        flashFreq: "keep",
                        flashSize: "keep",
                        eraseAll: fresh,
                        compress: true,
                        reportProgress(index, written, size) {
                                const done = before[index] + (written / size) * fileArray[index].data.length;
                                $("progress").value = done / total;
                                $("progress-text").textContent =
                                        `${d.images[index].name}: ${Math.round((100 * written) / size)}%`;
                        },
                        calculateMD5Hash: md5,
                });
                log(`Written and verified in ${((performance.now() - started) / 1000).toFixed(1)} s.`, "good");
                await closeTransport();
                setStep("done");
                await restartIntoFirmware();
        } catch (e) {
                log(`The install failed: ${e.message ?? e}`, "error");
                $("flash-problem").textContent = `${e.message ?? e}`;
                setStep("failed");
                await closeTransport();
        }
}

// Restarts the chip into its firmware: on USB-Serial/JTAG, DTR high holds the boot pin low and RTS
// high holds the chip in reset (docs/platforms/esp32p4.md). DTR is cleared first, so the lines
// never pass through "boot pin held" (with RTS still high from the port's open, clearing DTR is
// already a reset into the firmware), then RTS is pulsed with DTR low.
async function resetToFirmware(transport) {
        await transport.setDTR(false);
        await sleep(100);
        await transport.setRTS(true);
        await sleep(100);
        await transport.setRTS(false);
        return;
}

// After an install: the console opened first, so the boot is seen from its first line, then the
// chip restarted through it. In Chrome the chip sometimes came back in its ROM loader anyway (not
// reproduced with pyserial: 2026-10-30), which then says "waiting for download": so it's watched
// for, and the restart tried again.
async function restartIntoFirmware() {
        await openConsole();
        if (!state.consoleOpen) return;
        for (let attempt = 1; attempt <= 3; attempt++) {
                log(attempt === 1 ? "Restarting the board..." : `Restarting the board again (try ${attempt})...`);
                state.bootText = "";
                await resetToFirmware(state.consoleTransport);
                await sleep(2500);
                if (!/waiting for download/i.test(state.bootText)) {
                        log("The board is starting BMDS.", "good");
                        return;
                }
                log("The board came back in its download mode.", "error");
        }
        log("The board stays in its download mode: press its RESET button.", "error");
        return;
}

// ---- The console ----

// The board's port, found again if it went away: the board's RESET button restarts the whole chip,
// USB too, and the port comes back as a new device. Ports this page was given before come back
// without asking (getPorts()); otherwise the browser asks.
async function boardPort() {
        if (state.port && state.port.connected !== false) return state.port;
        const ports = await navigator.serial.getPorts();
        const known = ports.find((p) => p.getInfo().usbVendorId === EspressifVid && p.connected !== false);
        return known ?? (await navigator.serial.requestPort({ filters: [{ usbVendorId: EspressifVid }] }));
}

// The board's console speaks ANSI; this shows its text only.
const AnsiPattern = /\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[()][0-9A-Za-z]|\x1b[=>]/g;

function consoleWrite(text) {
        state.bootText = (state.bootText + text).slice(-4096);
        const out = $("console-output");
        out.textContent += text.replace(AnsiPattern, "").replace(/\r\n?/g, "\n");
        if (out.textContent.length > 200000) out.textContent = out.textContent.slice(-150000);
        out.scrollTop = out.scrollHeight;
}

async function openConsole() {
        if (state.consoleOpen) return;
        try {
                const port = await boardPort();
                state.port = port;
                state.consoleTransport = new Transport(port, false, false);
                await state.consoleTransport.connect(115200);
        } catch (e) {
                log(`Couldn't open the console: ${e.message ?? e}. It opens when the board comes back, ` +
                    "or use Open Console to try again.", "error");
                state.consoleTransport = null;
                state.wantConsole = true;
                $("console-open").hidden = false;
                return;
        }
        state.consoleOpen = true;
        state.wantConsole = false;
        $("console-open").hidden = true;
        $("console").hidden = false;
        const decoder = new TextDecoder();
        state.consoleTransport
                .rawRead(
                        (data) => consoleWrite(decoder.decode(data, { stream: true })),
                        () => !state.consoleOpen)
                .catch((e) => log(`The console closed: ${e.message ?? e}`, "error"))
                .finally(() => {
                        state.consoleOpen = false;
                        $("console-open").hidden = false;
                });
        return;
}

async function consoleSend(text) {
        if (!state.consoleTransport || !state.consoleOpen) return;
        await state.consoleTransport.write(new TextEncoder().encode(text));
        return;
}

async function closeConsole() {
        state.consoleOpen = false;
        if (state.consoleTransport) {
                try {
                        await state.consoleTransport.disconnect();
                } catch {
                        // already closed
                }
        }
        state.consoleTransport = null;
        return;
}

// ---- Start ----

function start() {
        if (!("serial" in navigator)) {
                setStep("unsupported");
                return;
        }
        setStep("firmware");
        $("folder").addEventListener("change", (e) =>
                pickFolder(e.target.files).catch((err) => log(err.message, "error")));
        $("connect").addEventListener("click", connect);
        $("install").addEventListener("click", install);
        $("console-open").addEventListener("click", openConsole);
        $("console-close").addEventListener("click", closeConsole);
        $("console-input").addEventListener("keydown", (e) => {
                if (e.key !== "Enter") return;
                consoleSend(e.target.value + "\r");
                e.target.value = "";
        });
        $("console-ctrlc").addEventListener("click", () => consoleSend("\x03"));
        // The board went away (its RESET button, a cable) and came back: follow it.
        navigator.serial.addEventListener("disconnect", (e) => {
                if (e.target !== state.port) return;
                log("The board's USB port went away.");
                state.wantConsole = state.consoleOpen;
                state.port = null;
                closeConsole();
        });
        navigator.serial.addEventListener("connect", (e) => {
                if (e.target.getInfo().usbVendorId !== EspressifVid) return;
                log("The board's USB port is back.");
                if (!state.wantConsole || state.transport) return;
                state.port = e.target;
                state.wantConsole = false;
                setTimeout(openConsole, 500);
        });
        if (location.protocol === "file:") {
                $("folder-picker").hidden = false;
        } else {
                fetchFirmware().catch(() => {
                        log("No published firmware here: pick a firmware folder instead.");
                        $("folder-picker").hidden = false;
                });
        }
        return;
}

start();
