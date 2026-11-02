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
// their offsets, the flash's size, and the chip revisions the build runs on. On a web server the
// descriptors are fetched (firmware/latest.json names one for each chip variant, written by
// tools/release site), and the board's chip revision picks one; opened as a file, the page can't
// fetch, so the user picks the firmware's folder. After the install the page can type the first
// settings into the board's console (the setup).

import { ESPLoader, Transport } from "./vendor/esptool-js/esptool.js";
import { md5 } from "./md5.js";

const EspressifVid = 0x303a;

const $ = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const state = {
        latest: null, // firmware/latest.json, when the page is on the site
        firmwares: [], // each { descriptor, read(image) -> Promise<Uint8Array> }
        firmware: null, // the one for the board's chip
        port: null,
        transport: null,
        loader: null,
        chip: null, // { description, revision, flashSize, mac }
        consoleOpen: false,
        consoleTransport: null,
        wantConsole: false, // reopen it when the board's port comes back
        bootText: "", // the console's latest text, for the restart's check
        watchText: "", // the console's latest text without ANSI, for the setup's commands
        watchTotal: 0, // how much text watchText has been given, trimmed or not
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

function revisionRange(r) {
        const maxSet = r.max !== 0 && r.max !== 0xffff;
        return maxSet ? `${revisionText(r.min)} to ${revisionText(r.max)}` : `${revisionText(r.min)} and later`;
}

function showFirmware() {
        const lines = state.firmwares.map((f) => {
                const d = f.descriptor;
                const bytes = d.images.reduce((n, im) => n + im.size, 0);
                return `${d.name} ${d.version} (${d.variant}): ${d.images.length} images, ` +
                       `${(bytes / 1048576).toFixed(1)} MB, for chip revisions ${revisionRange(d.chipRevision)}`;
        });
        $("firmware-info").textContent = lines.join("\n");
        $("connect").disabled = false;
}

// A link after the text in each element of @p cls: to @p href, named @p text.
function addLink(cls, href, text) {
        for (const el of document.querySelectorAll(cls)) {
                el.textContent = " (";
                const a = document.createElement("a");
                a.href = href;
                a.textContent = text;
                el.append(a, ")");
        }
}

async function fetchLatest() {
        const response = await fetch("firmware/latest.json");
        if (!response.ok) throw new Error(`firmware/latest.json: ${response.status}`);
        const latest = await response.json();
        if (latest.format !== 1) throw new Error("firmware/latest.json: a format this page doesn't read");
        state.latest = latest;
        if (latest.release) addLink(".release-link", latest.release, "the release's page");
        if (latest.packages) addLink(".packages-link", latest.packages, latest.packages.split("/").pop());
        return latest;
}

async function fetchFirmware() {
        const latest = state.latest ?? (await fetchLatest());
        for (const path of Object.values(latest.variants)) {
                const base = new URL(path, location.href);
                const response = await fetch(base);
                if (!response.ok) throw new Error(`${path}: ${response.status}`);
                const descriptor = checkDescriptor(await response.json());
                state.firmwares.push({
                        descriptor,
                        async read(image) {
                                const r = await fetch(new URL(image.file, base));
                                if (!r.ok) throw new Error(`${image.file}: ${r.status}`);
                                return new Uint8Array(await r.arrayBuffer());
                        },
                });
        }
        if (state.firmwares.length === 0) throw new Error("firmware/latest.json names no firmware");
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
        state.firmwares = [{
                descriptor,
                async read(image) {
                        return new Uint8Array(await byName.get(image.file).arrayBuffer());
                },
        }];
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
                const problems = [];
                state.firmware = state.firmwares.find((f) => runsOn(f.descriptor.chipRevision, revision)) ?? null;
                if (!state.firmware) {
                        const built = state.firmwares.map((f) => revisionRange(f.descriptor.chipRevision)).join(", ");
                        problems.push(
                                `This board's chip is revision ${revisionText(revision)}; this BMDS is built for ` +
                                        `${built}. Builds for revision 3 and later aren't published yet.`);
                } else {
                        const d = state.firmware.descriptor;
                        log(`The ${d.variant} build runs on this chip.`);
                        if (flashSize !== d.flashSize) {
                                problems.push(`This board has ${flashSize ?? "an unknown size of"} flash; BMDS needs ` +
                                              `${d.flashSize}.`);
                        }
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
                        showSetup();
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
        const plain = text.replace(AnsiPattern, "").replace(/\r\n?/g, "\n");
        state.watchText = (state.watchText + plain).slice(-65536);
        state.watchTotal += plain.length;
        const out = $("console-output");
        out.textContent += plain;
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

// ---- The setup ----

// Commands typed into the board's shell, each followed by an echo of its exit status that, as
// typed, doesn't match what it prints (the quotes split it), as tools/bmdsboard.py does.
const StatusPattern = /<<bmds-setup-status:(\d+)>>/;
const ReadyMarker = "<<bmds-setup-ready>>";

function showSetup() {
        if (!$("setup-zone").value) {
                try {
                        $("setup-zone").value = Intl.DateTimeFormat().resolvedOptions().timeZone ?? "";
                } catch {
                        // no zone known: left empty
                }
        }
        $("setup").hidden = false;
        return;
}

// Waits until @p pattern is in the console's text since @p since (a mark()). @return The match, or
// null on a timeout.
async function waitForText(pattern, since, ms) {
        const until = performance.now() + ms;
        for (;;) {
                const text = state.watchText.slice(Math.max(0, state.watchText.length - (state.watchTotal - since)));
                const m = typeof pattern === "string" ? (text.includes(pattern) ? [pattern] : null) : text.match(pattern);
                if (m) return m;
                if (performance.now() > until || !state.consoleOpen) return null;
                await sleep(100);
        }
}

// A mark in the console's text, for waitForText(): what comes after it.
function mark() {
        return state.watchTotal;
}

// Gets the shell's attention: Ctrl+C for any line being typed, then an echo, until it answers
// (the board may still be starting).
async function waitForShell(ms) {
        const until = performance.now() + ms;
        while (performance.now() < until && state.consoleOpen) {
                const since = mark();
                await consoleSend("\x03");
                await sleep(200);
                await consoleSend('echo "<<bmds-setup""-ready>>"\r');
                if (await waitForText(ReadyMarker, since, 3000)) return true;
        }
        return false;
}

// Types @p line into the shell; then each of @p answers after its prompt appears. @p shown is
// what the page says it typed. @return The exit status, or null on a timeout.
async function runOnBoard(line, shown, answers = []) {
        log(`Typing: ${shown}`);
        const since = mark();
        await consoleSend(`${line}; echo "<<bmds-setup""-status:"$?">>"\r`);
        for (const [prompt, answer] of answers) {
                if (!(await waitForText(prompt, since, 10000))) return null;
                await consoleSend(answer);
        }
        const m = await waitForText(StatusPattern, since, 20000);
        return m ? Number(m[1]) : null;
}

// For single quotes in the shell: none inside.
function shellWord(value) {
        return `'${value}'`;
}

async function applySetup() {
        const hostname = $("setup-hostname").value.trim();
        const zone = $("setup-zone").value.trim();
        const password = $("setup-password").value;
        const result = $("setup-result");
        result.className = "info";
        if (hostname && !/^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$/.test(hostname)) {
                result.textContent = "The name: letters, digits and - only, at most 63, not starting or ending with -.";
                return;
        }
        if (zone && !/^[A-Za-z0-9_+\-\/:,.<>]+$/.test(zone)) {
                result.textContent = "The time zone: a zone name like Europe/Berlin, or POSIX rules.";
                return;
        }
        if (password !== $("setup-password2").value) {
                result.textContent = "The passwords don't match.";
                return;
        }
        if (password && !/^[\x20-\x7e]{1,256}$/.test(password)) {
                result.textContent = "The password: printable ASCII only, at most 256 characters.";
                return;
        }
        if (!hostname && !zone && !password) {
                result.textContent = "Nothing to set.";
                return;
        }
        $("setup-apply").disabled = true;
        try {
                await openConsole();
                result.textContent = "Waiting for the board's shell...";
                if (!(await waitForShell(60000))) throw new Error("the board's shell didn't answer");
                const failed = [];
                if (hostname) {
                        const status = await runOnBoard(`hostname ${shellWord(hostname)}`, `hostname ${hostname}`);
                        if (status !== 0) failed.push("the name");
                }
                if (zone) {
                        const command = `sysconf set clock/zone ${shellWord(zone)} && sysconf save clock/zone`;
                        const status = await runOnBoard(command, command);
                        if (status !== 0) failed.push("the time zone");
                }
                if (password) {
                        // passwd asks twice and doesn't echo; the page doesn't show it either.
                        const status = await runOnBoard("passwd", "passwd (the password twice, not shown)", [
                                ["New password:", `${password}\r`],
                                ["Retype new password:", `${password}\r`],
                        ]);
                        if (status !== 0) failed.push("the password");
                }
                if (failed.length) throw new Error(`the board refused ${failed.join(", ")} (see the console)`);
                $("setup-password").value = "";
                $("setup-password2").value = "";
                result.className = "good";
                result.textContent = "Done." + (hostname ? ` The board is ${hostname}.local on the network.` : "");
                log("The setup is done.", "good");
        } catch (e) {
                result.className = "error";
                result.textContent = `The setup failed: ${e.message ?? e}`;
                log(`The setup failed: ${e.message ?? e}`, "error");
        } finally {
                $("setup-apply").disabled = false;
        }
        return;
}

// ---- Start ----

function start() {
        if (!("serial" in navigator)) {
                setStep("unsupported");
                if (location.protocol !== "file:") fetchLatest().catch(() => {});
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
        $("setup-apply").addEventListener("click", applySetup);
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
