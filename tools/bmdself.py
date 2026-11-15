# @file      bmdself.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# What the host tools need of an ELF file, by Python's standard library alone: its GNU build-id
# (devplan/debuginfo.md §3), so a debug ELF is used only for the build the board ran, and where a
# program's unstripped ELF is.

import glob
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SHT_NOTE = 7
NT_GNU_BUILD_ID = 3


def build_id(path):
    """The GNU build-id of the 32-bit little-endian ELF at @p path, in hex, or None."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        return None
    shoff, = struct.unpack_from("<I", data, 32)
    shentsize, shnum = struct.unpack_from("<HH", data, 46)
    for i in range(shnum):
        at = shoff + i * shentsize
        if at + 40 > len(data):
            break
        _, sh_type, _, _, offset, size = struct.unpack_from("<IIIIII", data, at)
        if sh_type != SHT_NOTE:
            continue
        pos, end = offset, offset + size
        while pos + 12 <= end:
            namesz, descsz, kind = struct.unpack_from("<III", data, pos)
            name = data[pos + 12:pos + 12 + namesz]
            desc_at = pos + 12 + ((namesz + 3) & ~3)
            if kind == NT_GNU_BUILD_ID and name == b"GNU\0":
                return data[desc_at:desc_at + descsz].hex()
            pos = desc_at + ((descsz + 3) & ~3)
    return None


def matches(path, wanted):
    """True if @p path's build-id is @p wanted (hex; a prefix of it is enough: the profiler keeps
    20 bytes), or if the log gave none to check against."""
    if not wanted:
        return True
    have = build_id(path)
    return have is not None and (have.startswith(wanted) or wanted.startswith(have))


def find_program(name):
    """The unstripped ELF of a program the board ran as NAME: the firmware's ROM build
    (build/rom/dist), the distribution's (build-dist), a link's target followed; else the SDK
    examples (build-host/sdk-examples)."""
    for dist in ("build/rom/dist", "build-dist"):
        for link in glob.glob(os.path.join(ROOT, dist, "stage", "*", "bin", name)):
            target = os.path.basename(os.path.realpath(link))
            package = link.split(os.sep)[-3]
            for top, _, files in os.walk(os.path.join(ROOT, dist, "build", package)):
                # A build that strips its own output keeps the whole one beside it (BusyBox).
                for candidate in (target + "_unstripped", target):
                    if candidate in files:
                        return os.path.join(top, candidate)
    stem = name[:-4] if name.endswith(".elf") else name
    for candidate in (stem + ".debug.elf", stem + ".elf"):
        example = os.path.join(ROOT, "build-host", "sdk-examples", candidate)
        if os.path.exists(example):
            return example
    return None
