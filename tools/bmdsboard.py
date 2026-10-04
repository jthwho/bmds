# @file      bmdsboard.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Talking to a BMDS board's shell over its serial console, for bmds-run and
# bmds-compat: run a command line and get its output and exit status, and
# send files with YMODEM to the shell's `rz` (no lrzsz needed). Needs
# pyserial (ESP-IDF's Python environment has it).
#
# The shell echoes what it is sent and its prompt is the user's, so neither
# is parsed: each command is bracketed by markers that `echo` prints, written
# with an empty "" in the middle so the echoed command line never matches.

import glob
import os
import select
import shlex
import sys
import time

import serial

START = b"<<bmds-run-start>>"
STATUS = b"<<bmds-run-status:"
ABANDONED = b"^C\r\n"  # what the shell says when Ctrl-C abandons a command line


class BoardError(Exception):
    pass


def default_port():
    """$ESPPORT, else the one /dev/esp-* link, else /dev/ttyACM0."""
    port = os.environ.get("ESPPORT")
    if port:
        return port
    links = sorted(glob.glob("/dev/esp-*"))
    if len(links) == 1:
        return links[0]
    return "/dev/ttyACM0"


def quote(word):
    """One word for the BMDS shell (single quotes, as in sh)."""
    return shlex.quote(word)


def marker(text):
    """An echo argument printing <<bmds-run-TEXT>> but not matching it as typed."""
    return '"<<bmds-run""-%s>>"' % text


def crc16(data):
    """CRC-16/XMODEM (polynomial 0x1021, initial 0), as YMODEM uses."""
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


SOH, STX, EOT, ACK, NAK, CAN = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18


class Board:
    def __init__(self, port=None):
        self.port = port or default_port()
        try:
            self.serial = serial.Serial(self.port, 115200, timeout=0.05)
        except serial.SerialException as e:
            raise BoardError(str(e))
        self.pending = b""

    def close(self):
        self.serial.close()

    # Reading -----------------------------------------------------------

    def read_until(self, token, seconds):
        """Reads until @p token has arrived; returns what came before it (the
        rest stays pending). Raises BoardError after @p seconds."""
        end = time.monotonic() + seconds
        while token not in self.pending:
            if time.monotonic() > end:
                raise BoardError("no answer from the board on %s" % self.port)
            self.pending += self.serial.read(4096)
        before, _, self.pending = self.pending.partition(token)
        return before

    def drain(self, quiet=0.2):
        """Throws away input until the line has been quiet for @p quiet seconds."""
        last = time.monotonic()
        while time.monotonic() - last < quiet:
            if self.serial.read(4096):
                last = time.monotonic()
        self.pending = b""

    # The shell ---------------------------------------------------------

    def sync(self):
        """Gets a prompt: cancels any line being typed, then checks that the
        shell answers (twice: the first line after a boot can be lost)."""
        for attempt in range(3):
            self.serial.write(b"\x03")
            self.drain()
            self.serial.write(("echo %s\r" % marker("ready")).encode())
            try:
                self.read_until(b"<<bmds-run-ready>>", 3)
                self.drain(0.1)
                return
            except BoardError:
                if attempt == 2:
                    raise

    def start(self, line):
        """Sends @p line (shell syntax) bracketed by the markers; the output
        then comes from read_output()."""
        command = "echo %s; %s; echo %s$?%s\r" % (marker("start"), line, '"<<bmds-run""-status:"', '">>"')
        self.serial.write(command.encode())
        self.read_until(START + b"\r\n", 5)

    def read_output(self, sink, seconds=None, keys=None):
        """Passes the command's output to @p sink until it ends; returns its
        status. With @p keys (a file descriptor), what is typed there goes to
        the board. Raises BoardError on a timeout."""
        end = None if seconds is None else time.monotonic() + seconds
        held = b""  # a tail that may be the start of the status marker
        interrupted = False
        while True:
            data = held + self.pending + self.serial.read(4096)
            self.pending = b""
            # A Ctrl-C that ends the program makes the shell abandon the rest
            # of the line, the status marker too; it says ^C instead.
            if interrupted and ABANDONED in data:
                sink(data[: data.find(ABANDONED)])
                self.drain(0.2)
                return 130
            at = data.find(STATUS)
            if at >= 0:
                sink(data[:at])
                rest = data[at + len(STATUS):]
                while b">>" not in rest:
                    rest += self.serial.read(64)
                status, _, self.pending = rest.partition(b">>")
                return int(status)
            keep = len(STATUS) - 1
            for n in range(min(keep, len(data)), 0, -1):
                if STATUS.startswith(data[-n:]) or (interrupted and ABANDONED.startswith(data[-n:])):
                    break
            else:
                n = 0
            sink(data[: len(data) - n])
            held = data[len(data) - n:]
            if keys is not None and select.select([keys], [], [], 0)[0]:
                typed = os.read(keys, 256)
                if b"\x1d" in typed:  # Ctrl-]: stop it and leave
                    self.serial.write(b"\x03\x03")
                    raise KeyboardInterrupt
                interrupted = interrupted or b"\x03" in typed
                self.serial.write(typed)
            if end is not None and time.monotonic() > end:
                self.serial.write(b"\x03\x03")
                raise BoardError("timed out")

    def run(self, line, seconds=60):
        """Runs @p line; returns (output with LF line ends, status)."""
        out = []
        self.start(line)
        status = self.read_output(out.append, seconds)
        return b"".join(out).replace(b"\r\n", b"\n"), status

    # Files -------------------------------------------------------------

    def send_files(self, paths, directory):
        """Sends files with YMODEM into @p directory on the board (made if
        missing), replacing files of the same names."""
        line = "mkdir -p %s && (cd %s && rz -y)" % (quote(directory), quote(directory))
        self.start(line)
        # rz says it's waiting (with no capital C in the message), then asks with C.
        self.read_until(b"to cancel", 10)
        for path in paths:
            with open(path, "rb") as f:
                data = f.read()
            self._send_file(os.path.basename(path), data, int(os.path.getmtime(path)))
        self._wait_for_c()
        self._send_block(0, bytes(128))  # the empty block 0 ends the batch
        report = []
        status = self.read_output(report.append, 30)
        if status != 0:
            raise BoardError("rz failed: " + b"".join(report).decode(errors="replace").strip())

    def _wait_for_c(self, seconds=10):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            data = self.pending + self.serial.read(1)
            self.pending = b""
            for i, b in enumerate(data):
                if b == ord("C"):
                    self.pending = data[i + 1:]
                    return
                if b == CAN:
                    raise BoardError("the board cancelled the transfer")
        raise BoardError("the board's rz did not start")

    def _answer(self, seconds=10):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            data = self.pending + self.serial.read(1)
            self.pending = b""
            for i, b in enumerate(data):
                if b in (ACK, NAK, CAN):
                    self.pending = data[i + 1:]
                    return b
        return None

    def _send_block(self, seq, payload):
        head = SOH if len(payload) == 128 else STX
        crc = crc16(payload)
        packet = bytes([head, seq & 0xFF, 0xFF - (seq & 0xFF)]) + payload + bytes([crc >> 8, crc & 0xFF])
        for _ in range(10):
            self.serial.write(packet)
            answer = self._answer()
            if answer == ACK:
                return
            if answer == CAN:
                raise BoardError("the board cancelled the transfer")
        raise BoardError("block %d not acknowledged" % seq)

    def _send_file(self, name, data, mtime):
        self._wait_for_c()
        info = name.encode() + b"\0" + ("%d %o 0" % (len(data), mtime)).encode()
        if len(info) > 1024:
            raise BoardError("%s: name too long" % name)
        size = 128 if len(info) <= 128 else 1024
        self._send_block(0, info.ljust(size, b"\0"))
        self._wait_for_c()
        for seq, at in enumerate(range(0, len(data), 1024), start=1):
            self._send_block(seq, data[at:at + 1024].ljust(1024, b"\x1a"))
        # EOT: the receiver answers the first with NAK, on purpose.
        for _ in range(10):
            self.serial.write(bytes([EOT]))
            if self._answer() == ACK:
                return
        raise BoardError("end of %s not acknowledged" % name)


def stdout_sink():
    """Writes output to stdout: as it comes on a terminal, with LF line ends otherwise."""
    out = sys.stdout.buffer
    if sys.stdout.isatty():
        def sink(data):
            out.write(data)
            out.flush()
    else:
        held = [b""]

        def sink(data):
            data = held[0] + data
            # A CR at the end may be half of a CR LF.
            held[0] = b"\r" if data.endswith(b"\r") else b""
            if held[0]:
                data = data[:-1]
            out.write(data.replace(b"\r\n", b"\n"))
            out.flush()
    return sink
