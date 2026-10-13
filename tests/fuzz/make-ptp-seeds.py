#!/usr/bin/env python3
# @file      make-ptp-seeds.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Seeds for ptp_fuzz (tests/fuzz/ptp.cpp): inputs in its record layout that take the port where
# the fuzzer can't easily get alone: following a master (two Announces, Syncs with Follow_Ups,
# Delay_Resps to its requests), and being master (Delay_Reqs to answer).
#
#   tests/fuzz/make-ptp-seeds.py DIR
#
# Python's standard library only.

import os
import struct
import sys

MASTER = bytes([0x02, 0x00, 0x00, 0xFF, 0xFE, 0x00, 0x00, 0x09])
OURS = bytes([0x02, 0x00, 0x00, 0xFF, 0xFE, 0x00, 0x00, 0x01])
OTHER = bytes([0x02, 0x00, 0x00, 0xFF, 0xFE, 0x00, 0x00, 0x05])
EPOCH = 1_800_000_000


def header(mtype, length, seq, control, log, flags=0, source=MASTER):
    return struct.pack(">BBHBBHq4s8sHHBb", mtype, 2, length, 0, 0, flags, 0, b"\0" * 4, source, 1, seq,
                       control, log)


def stamp(seconds, ns):
    return struct.pack(">HIL", seconds >> 32, seconds & 0xFFFFFFFF, ns)


def announce(seq, priority1=100):
    body = stamp(EPOCH, 0) + struct.pack(">hBBBBHB8sHB", 37, 0, priority1, 248, 0xFE, 0xFFFF, 128, MASTER, 0, 0xA0)
    return header(0xB, 64, seq, 5, 1, 0x0004 | 0x0008) + body


def sync(seq):
    return header(0x0, 44, seq, 0, -3, 0x0200) + stamp(EPOCH, 0)


def follow_up(seq, ns):
    return header(0x8, 44, seq, 2, -3) + stamp(EPOCH + seq // 8, ns)


def delay_resp(seq, ns):
    return header(0x9, 54, seq, 3, -3) + stamp(EPOCH + seq // 8, ns) + OURS + struct.pack(">H", 1)


def delay_req(seq):
    return header(0x1, 44, seq, 1, 0x7F, source=OTHER) + stamp(EPOCH, 0)


def message(m):
    return bytes([0, len(m)]) + m


def wait(ms):
    out = b""
    while ms > 0:
        if ms >= 8 * 255:
            out += bytes([1 | 4, 255])
            ms -= 8 * 255
        else:
            step = min(ms, 255)
            out += bytes([1, step])
            ms -= step
    return out


def slave():
    out = message(announce(0)) + wait(2000) + message(announce(1))
    for k in range(48):
        out += message(sync(k)) + message(follow_up(k, 1000 * k))
        # The port's Delay_Reqs go on a random timer (one per 125 ms on average): answer the
        # sequence numbers around where it is, so one matches.
        out += bytes([2])
        for seq in range(max(0, k - 3), k + 2):
            out += message(delay_resp(seq, 125_000_000 * (k % 8) + 30_000))
        out += wait(125)
        if k % 16 == 15:
            out += message(announce(2 + k // 16))
    return out


def master():
    out = wait(12_000)
    for k in range(16):
        out += message(delay_req(k)) + bytes([2]) + wait(125)
    return out


def main():
    if len(sys.argv) != 2:
        print("usage: make-ptp-seeds.py DIR", file=sys.stderr)
        return 2
    os.makedirs(sys.argv[1], exist_ok=True)
    seeds = {"slave": slave(), "master": master(), "announce": announce(0), "delay_resp": delay_resp(3, 5)}
    for name, data in seeds.items():
        with open(os.path.join(sys.argv[1], name), "wb") as f:
            f.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
