#!/usr/bin/env python3
# @file      mkrom.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Builds what the ROM holds (devplan/rom.md): the image (BMRO), the bmds icon
# theme as one file of decoded pixels (BMIC) with its licenses, the IconId
# header generated from the same list, and checks that every icon name BMDS's
# files ask for is in the theme.
#
#   mkrom.py image OUT [--manifest-header H] [--max-bytes N] [--log-bytes N] [--file DEST=SRC]
#                      [--dir DEST=SRC] [--link DEST=TARGET] [--align SIZE=GLOB]...
#   mkrom.py icons LIST ROOT OUT_ICONS OUT_LICENSES
#   mkrom.py iconid LIST OUT_HEADER
#   mkrom.py check LIST DIR...      (the Icon= of every .desktop file under DIR)
#
# The formats (little-endian; on-disk formats, frozen once shipped; the readers are
# bmds_core's RomImage and IconPack):
#
# BMRO, the partition (format 2, devplan/rom.md §12c):
#   header, 64 bytes at 0: "BMRO", u16 format (2), u16 header size (64), u32 the log's offset
#                     (a sector, 4096 here), u32 the log's size (whole sectors), u32 where the
#                     files' space starts (64 KB-aligned here), u32 flags (0), 36 bytes 0,
#                     u32 CRC-32 of the header's first 60 bytes
#   the log: tree versions, each starting on a 4 KB sector; the rest erased (0xff). A version:
#     header, 80 bytes: "BMRT", u16 header size (80), u16 entry size (64), u32 the version's
#                     size (all of it, its CRC included), u32 sequence (newer is larger),
#                     u32 entry count, u32 the paths' offset (from the version's start),
#                     u32 the paths' size, u32 flags (0), i64 when written (Unix seconds),
#                     32 bytes the firmware build it needs (0: any), 8 bytes 0
#     entries, 64 bytes, one a file, sorted by path (bytes): u32 path offset, u16 path length,
#                     u8 alignment (log2), u8 flags (1 System, 2 the user's), u32 offset in
#                     the partition, u32 size, i64 time (Unix seconds), 32 bytes BLAKE2s-256
#                     of its bytes, 8 bytes 0
#     the paths (whole, from the root, no leading '/'; directories are implied), then
#     u32 CRC-32 of the version before it.
#   the files' bytes, each at its alignment (64 bytes unless --align says more); files
#   aligned to 64 KB first, each taking whole 64 KB pages, then the rest. Unused bytes are
#   0xff (erased flash).
#   The manifest (--manifest-header): the system files as a C++ header (RomManifestFiles, of
#   bmds::RomManifestEntry: path, size, flags, BLAKE2s-256), sorted by path, which the firmware
#   compares with what the ROM holds (devplan/rom.md §12f).
#
# BMIC, the icon theme:
#   header, 48 bytes: "BMIC", u16 format (1), u16 header size (48), u32 file
#                     size, u32 icon count, 4 x u16 sides (ascending, 0 unused),
#                     u32 table offset, u32 index offset, u32 names' offset,
#                     u32 names' size, 8 bytes 0
#   table, 24 bytes an icon by id: u32 name offset, u16 name length, u16 0,
#                     4 x u32 pixel offset (0: not at that size)
#   index: u16 ids sorted by name; names; pixels: 0xAARRGGBB words, rows top
#   first, each icon on a 64-byte boundary.
#
# Python's standard library only. Outputs are rewritten only when they change.

import fnmatch
import hashlib
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))

ROM_HEADER = 64
VERSION_HEADER = 80
ROM_ENTRY = 64
SECTOR = 4096
PAGE = 65536
DATA_ALIGN = 64
LOG_BYTES = 124 * 1024  # with the header's sector, the files start at 128 KB
MAX_PATH = 255
MAX_ALIGN = 1 << 24
FLAG_SYSTEM = 1
LINK_COOKIE = b"!<symlink>"

ICON_HEADER = 48
ICON_ROW = 24
ICON_SIDES = [16, 32]
MAX_SIZES = 4
MAX_ICON_NAME = 63

# The themes icons come from: their rendered PNGs (tools/render-icons) and license.
THEMES = {
    "flat-color": ("thirdparty/flat-color-icons", "LICENSE.md", "icons8's Flat Color Icons"),
    "glyphs": ("thirdparty/glyphs", "LICENSE", "Glyphs (gorango/glyphs, core set, coloured)"),
    "bmds": ("rom/icons", "../../LICENSE", "BMDS's own (rom/icons/svg, drawn after flat-color)"),
}


def die(message):
    print("mkrom: " + message, file=sys.stderr)
    sys.exit(1)


def write_if_changed(path, data):
    if isinstance(data, str):
        data = data.encode()
    try:
        with open(path, "rb") as f:
            if f.read() == data:
                return
    except OSError:
        pass
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def align(n, a):
    return (n + a - 1) // a * a


# --- The image -----------------------------------------------------------------------------


def check_path(path):
    encoded = path.encode()
    parts = path.split("/")
    if not path or len(encoded) > MAX_PATH or "\0" in path or any(p in ("", ".", "..") for p in parts):
        die("%r: not a path a ROM file may have (1 to %d bytes, no empty, . or .. part)" % (path, MAX_PATH))


def build_image(out, files, dirs, links, aligns, manifest_header, max_bytes, log_bytes):
    entries = {}  # path: [data, flags, alignment]
    newest = 0

    def add(dest, data, flags):
        path = dest.strip("/")
        check_path(path)
        if path in entries:
            die("%s given twice" % path)
        alignment = DATA_ALIGN
        for size, glob in aligns:
            if fnmatch.fnmatch(path, glob):
                alignment = max(alignment, size)
        entries[path] = (data, flags, alignment)

    def add_file(dest, src):
        nonlocal newest
        with open(src, "rb") as f:
            data = f.read()
        newest = max(newest, int(os.stat(src).st_mtime))
        add(dest, data, 0)

    for dest, src in files:
        add_file(dest, src)
    for dest, src in dirs:
        if not os.path.isdir(src):
            die("%s isn't a directory" % src)
        for base, subdirs, names in os.walk(src):
            subdirs[:] = sorted(d for d in subdirs if not d.startswith("."))
            for name in sorted(names):
                if name.startswith("."):
                    continue
                rel = os.path.relpath(os.path.join(base, name), src).replace(os.sep, "/")
                add_file(dest.rstrip("/") + "/" + rel if dest.strip("/") else rel, os.path.join(base, name))
    for dest, target in links:
        # Cygwin's system-file link, as the VFS writes one (devplan/storage.md §4).
        add(dest, LINK_COOKIE + b"\xff\xfe" + target.encode("utf-16-le") + b"\0\0", FLAG_SYSTEM)
    newest = int(os.environ.get("SOURCE_DATE_EPOCH", newest))

    paths = sorted(entries, key=lambda p: p.encode())
    for path in paths:
        parts = path.split("/")
        for i in range(1, len(parts)):
            if "/".join(parts[:i]) in entries:
                die("%s: %s is a file" % (path, "/".join(parts[:i])))

    # The files' places: those on 64 KB pages first (each its own pages), then by alignment.
    if log_bytes <= 0 or log_bytes % SECTOR:
        die("--log-bytes: whole %d-byte sectors" % SECTOR)
    log_at = SECTOR
    data_offset = align(log_at + log_bytes, PAGE)
    offset = data_offset
    place_at = {}
    for path in sorted(paths, key=lambda p: (-entries[p][2], p.encode())):
        data, _, alignment = entries[path]
        offset = align(offset, alignment)
        place_at[path] = offset
        offset += len(data)
        if alignment >= PAGE:
            offset = align(offset, PAGE)
    size = max(offset, data_offset)
    if size > 0xFFFFFFFF:
        die("the image is over 4 GB")
    if max_bytes is not None and size > max_bytes:
        die("the image is %d bytes: its partition holds %d" % (size, max_bytes))

    # The tree version: header, entries, paths, CRC.
    path_bytes = bytearray()
    table = bytearray()
    manifest = []
    for path in paths:
        data, flags, alignment = entries[path]
        encoded = path.encode()
        digest = hashlib.blake2s(data).digest()
        table += struct.pack("<IHBBIIq32s8x", len(path_bytes), len(encoded), alignment.bit_length() - 1, flags,
                             place_at[path], len(data), newest, digest)
        path_bytes += encoded
        manifest.append((path, len(data), flags, digest))
    paths_at = VERSION_HEADER + len(table)
    record_bytes = paths_at + len(path_bytes) + 4
    if record_bytes > log_bytes:
        die("the tree (%d bytes) doesn't fit the log (%d)" % (record_bytes, log_bytes))
    record = bytearray(struct.pack("<4sHHIIIIIIq32s8x", b"BMRT", VERSION_HEADER, ROM_ENTRY, record_bytes, 1,
                                   len(paths), paths_at, len(path_bytes), 0, newest, bytes(32)))
    record += table + path_bytes
    record += struct.pack("<I", zlib.crc32(bytes(record)) & 0xFFFFFFFF)

    image = bytearray(b"\xff" * size)
    header = struct.pack("<4sHHIIII36x", b"BMRO", 2, ROM_HEADER, log_at, log_bytes, data_offset, 0)
    image[0:ROM_HEADER] = header + struct.pack("<I", zlib.crc32(header) & 0xFFFFFFFF)
    image[log_at:log_at + len(record)] = record
    for path in paths:
        data = entries[path][0]
        image[place_at[path]:place_at[path] + len(data)] = data
    write_if_changed(out, bytes(image))
    if manifest_header:
        write_if_changed(manifest_header, manifest_text(manifest))


def manifest_text(manifest):
    rows = ["        {%s, %d, %d, {%s}}," % (c_string(path), size, flags, ", ".join("0x%02x" % b for b in digest))
            for path, size, flags, digest in manifest]
    if not rows:
        rows = ['        {"", 0, 0, {}}, // none: an array has at least one']
    return ("// Generated by tools/mkrom.py: do not edit.\n"
            "// The ROM's system files as the firmware was built with them (devplan/rom.md §12f).\n"
            "#pragma once\n#include <cstddef>\n#include <bmds/rommanifest.h>\n\nnamespace {\n\n"
            "        constexpr size_t RomManifestCount = %d;\n\n"
            "        constexpr bmds::RomManifestEntry RomManifestFiles[] = {\n%s\n        };\n\n} // namespace\n"
            % (len(manifest), "\n".join("        " + r for r in rows)))


def c_string(text):
    out = '"'
    for b in text.encode():
        if b in (0x22, 0x5c) or b < 0x20 or b > 0x7e:
            out += '\\%03o' % b
        else:
            out += chr(b)
    return out + '"'


def parse_size(text):
    t = text.strip().lower()
    scale = 1
    if t.endswith("k"):
        scale, t = 1024, t[:-1]
    elif t.endswith("m"):
        scale, t = 1024 * 1024, t[:-1]
    try:
        n = int(t, 0) * scale
    except ValueError:
        die("%s: not a size" % text)
    return n


def alignments(values):
    out = []
    for v in values:
        if "=" not in v:
            die("--align %s: SIZE=GLOB" % v)
        size, glob = v.split("=", 1)
        n = parse_size(size)
        if n < 1 or n > MAX_ALIGN or n & (n - 1):
            die("--align %s: a power of two up to %d" % (size, MAX_ALIGN))
        out.append((n, glob.strip("/")))
    return out


# --- PNG (8-bit RGBA, not interlaced: what tools/render-icons writes) ----------------------


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        die("%s isn't a PNG (an LFS pointer? git lfs pull)" % path)
    at = 8
    width = height = 0
    idat = bytearray()
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        at += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or color != 6 or interlace != 0:
                die("%s: only 8-bit RGBA, not interlaced, is read" % path)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    raw = zlib.decompress(bytes(idat))
    stride = width * 4
    rows = []
    previous = bytearray(stride)
    for y in range(height):
        line = raw[y * (stride + 1):(y + 1) * (stride + 1)]
        kind, cur = line[0], bytearray(line[1:])
        for x in range(stride):
            a = cur[x - 4] if x >= 4 else 0
            b = previous[x]
            c = previous[x - 4] if x >= 4 else 0
            if kind == 1:
                cur[x] = (cur[x] + a) & 0xFF
            elif kind == 2:
                cur[x] = (cur[x] + b) & 0xFF
            elif kind == 3:
                cur[x] = (cur[x] + (a + b) // 2) & 0xFF
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                cur[x] = (cur[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
            elif kind != 0:
                die("%s: filter %d" % (path, kind))
        rows.append(cur)
        previous = cur
    return width, height, rows


def argb_words(rows, mirror):
    # 0xAARRGGBB words, little-endian: B, G, R, A in memory.
    out = bytearray()
    for row in rows:
        pixels = [row[i:i + 4] for i in range(0, len(row), 4)]
        if mirror:
            pixels.reverse()
        for r, g, b, a in pixels:
            out += bytes((b, g, r, a))
    return out


# --- The icon list ---------------------------------------------------------------------------


class Icon:
    def __init__(self, name, theme, source, options, line):
        self.name, self.theme, self.source, self.options, self.line = name, theme, source, options, line


def enumerator(name):
    return "".join(part[:1].upper() + part[1:] for part in name.split("-"))


def read_list(path):
    icons = []
    seen = {}
    enums = {}
    with open(path) as f:
        for number, text in enumerate(f, 1):
            text = text.split("#", 1)[0].strip()
            if not text:
                continue
            fields = text.split()
            where = "%s:%d" % (path, number)
            if len(fields) < 3:
                die("%s: NAME THEME SOURCE [mirror]" % where)
            name, theme, source, options = fields[0], fields[1], fields[2], fields[3:]
            if not re.fullmatch(r"[a-z0-9]+(-[a-z0-9]+)*", name) or len(name) > MAX_ICON_NAME:
                die("%s: %s isn't a name an icon may have" % (where, name))
            if name in seen:
                die("%s: %s is already line %d" % (where, name, seen[name]))
            e = enumerator(name)
            if e in enums:
                die("%s: %s and %s make the same IconId" % (where, name, enums[e]))
            if theme not in THEMES:
                die("%s: no theme %s (%s)" % (where, theme, ", ".join(THEMES)))
            for o in options:
                if o != "mirror":
                    die("%s: no option %s" % (where, o))
            seen[name] = number
            enums[e] = name
            icons.append(Icon(name, theme, source, options, number))
    if not icons:
        die("%s: no icons" % path)
    if len(icons) > 65535:
        die("%s: more than 65535 icons" % path)
    return icons


def build_icons(list_path, root, out_icons, out_licenses):
    icons = read_list(list_path)
    names = bytearray()
    name_at = []
    for icon in icons:
        name_at.append((len(names), len(icon.name)))
        names += icon.name.encode()
    table_at = ICON_HEADER
    index_at = table_at + len(icons) * ICON_ROW
    names_at = align(index_at + 2 * len(icons), 4)
    offset = align(names_at + len(names), DATA_ALIGN)
    pixels = []
    offsets = []
    for icon in icons:
        row = []
        for side in ICON_SIDES:
            directory = THEMES[icon.theme][0]
            png = os.path.join(root, directory, "png", "%dx%d" % (side, side), icon.source + ".png")
            if not os.path.isfile(png):
                die("%s line %d: %s has no %s at %d pixels (%s)" % (list_path, icon.line, icon.theme, icon.source,
                                                                    side, png))
            width, height, rows = read_png(png)
            if width != side or height != side:
                die("%s is %dx%d, not %dx%d" % (png, width, height, side, side))
            offset = align(offset, DATA_ALIGN)
            row.append(offset)
            words = argb_words(rows, "mirror" in icon.options)
            pixels.append((offset, words))
            offset += len(words)
        offsets.append(row)
    size = offset
    blob = bytearray(size)
    sides = ICON_SIDES + [0] * (MAX_SIZES - len(ICON_SIDES))
    struct.pack_into("<4sHHII4HIIII8x", blob, 0, b"BMIC", 1, ICON_HEADER, size, len(icons), *sides, table_at,
                     index_at, names_at, len(names))
    for i, icon in enumerate(icons):
        at, length = name_at[i]
        row = offsets[i] + [0] * (MAX_SIZES - len(offsets[i]))
        struct.pack_into("<IHH4I", blob, table_at + i * ICON_ROW, at, length, 0, *row)
    order = sorted(range(len(icons)), key=lambda i: icons[i].name.encode())
    for k, i in enumerate(order):
        struct.pack_into("<H", blob, index_at + 2 * k, i)
    blob[names_at:names_at + len(names)] = names
    for at, words in pixels:
        blob[at:at + len(words)] = words
    write_if_changed(out_icons, bytes(blob))

    # Both upstreams are MIT: their notices, and where each icon came from.
    text = ["The bmds icon theme (devplan/rom.md), made from these icon sets by rom/icons.list.", ""]
    for theme, (directory, license_file, title) in THEMES.items():
        with open(os.path.join(root, directory, license_file)) as f:
            text += ["=" * 72, "%s (%s)" % (title, theme), "=" * 72, f.read().rstrip(), ""]
    text += ["=" * 72, "Where each icon came from", "=" * 72]
    for icon in icons:
        text.append("%-32s %-11s %s%s" % (icon.name, icon.theme, icon.source,
                                          " (mirrored)" if "mirror" in icon.options else ""))
    write_if_changed(out_licenses, "\n".join(text) + "\n")


def build_iconid(list_path, out):
    icons = read_list(list_path)
    lines = ["// Generated by tools/mkrom.py from rom/icons.list: do not edit.",
             "#pragma once", "", "#include <cstdint>", "#include <bmds/config.h>", "", "BMDS_NAMESPACE_BEGIN", "",
             "/**",
             " * @brief An icon of the bmds theme (devplan/rom.md §3f), by number: an index",
             " *        into the theme's file. Numbered in rom/icons.list's order, which is",
             " *        only added to, so a number never changes once shipped.",
             " */",
             "enum class IconId : uint16_t {"]
    for i, icon in enumerate(icons):
        lines.append("        %s = %d, ///< %s" % (enumerator(icon.name), i, icon.name))
    lines += ["};", "", "/** @brief The number of icons this firmware knows. */",
              "inline constexpr uint16_t IconIdCount = %d;" % len(icons), "", "BMDS_NAMESPACE_END", ""]
    write_if_changed(out, "\n".join(lines))


def check(list_path, dirs):
    known = {icon.name for icon in read_list(list_path)}
    missing = []
    for top in dirs:
        for base, subdirs, names in os.walk(top):
            subdirs.sort()
            for name in sorted(names):
                if not name.endswith(".desktop"):
                    continue
                path = os.path.join(base, name)
                with open(path) as f:
                    for line in f:
                        if not line.startswith("Icon="):
                            continue
                        icon = line[5:].strip()
                        # A path names a file of its own, not a theme's icon.
                        if icon and "/" not in icon and icon not in known:
                            missing.append("%s: Icon=%s" % (os.path.relpath(path, ROOT), icon))
    if missing:
        die("icons not in %s:\n  %s" % (os.path.relpath(list_path, ROOT), "\n  ".join(missing)))


def pairs(values, what):
    out = []
    for v in values:
        if "=" not in v:
            die("%s %s: DEST=SOURCE" % (what, v))
        dest, src = v.split("=", 1)
        out.append((dest.strip("/"), src))
    return out


def main(argv):
    if len(argv) < 2:
        die("image | icons | iconid | check (see the top of tools/mkrom.py)")
    command, args = argv[1], argv[2:]
    if command == "image":
        if not args:
            die("image OUT ...")
        out, rest = args[0], args[1:]
        files, dirs, links, aligns, manifest_header, max_bytes, log_bytes = [], [], [], [], None, None, LOG_BYTES
        i = 0
        while i < len(rest):
            option = rest[i]
            if i + 1 >= len(rest):
                die("%s needs a value" % option)
            value = rest[i + 1]
            if option == "--file":
                files.append(value)
            elif option == "--dir":
                dirs.append(value)
            elif option == "--link":
                links.append(value)
            elif option == "--align":
                aligns.append(value)
            elif option == "--manifest-header":
                manifest_header = value
            elif option == "--max-bytes":
                max_bytes = parse_size(value)
            elif option == "--log-bytes":
                log_bytes = parse_size(value)
            else:
                die("no option %s" % option)
            i += 2
        build_image(out, pairs(files, "--file"), pairs(dirs, "--dir"), pairs(links, "--link"), alignments(aligns),
                    manifest_header, max_bytes, log_bytes)
    elif command == "icons" and len(args) == 4:
        build_icons(*args)
    elif command == "iconid" and len(args) == 2:
        build_iconid(*args)
    elif command == "check" and len(args) >= 2:
        check(args[0], args[1:])
    else:
        die("%s: wrong arguments (see the top of tools/mkrom.py)" % command)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
