#!/usr/bin/env python3
# @file      guigen.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# The GUI classes' generator (devplan/guiprograms.md §10): reads sdk/gui/classes.def, the class
# ABI's one source, and writes the C wrappers (bmds/guiclasses.h) or the lock's lines
# (tests/scripts/gui.lock).
#
#   guigen.py header CLASSES.DEF OUT.h     the C wrappers
#   guigen.py lock CLASSES.DEF             the lock's lines, on stdout

import re
import sys

ENTRY = re.compile(r"^\s*(GUI_CLASS|GUI_PROP|GUI_VALUES|GUI_METHOD|GUI_EVENT)\((.*)\)\s*$")

TAGS = ("Int", "Long", "Bool", "String", "Atom", "Id", "Rect", "Color", "List")


def param(tag, n):
    """A tag's C parameters named after @p n, and the expression making its value."""
    return {
        "Int": ("int32_t %s" % n, "bmds_gui_int(%s)" % n),
        "Long": ("int64_t %s" % n, "bmds_gui_long(%s)" % n),
        "Bool": ("int %s" % n, "bmds_gui_bool(%s)" % n),
        "String": ("const char *%s" % n, "bmds_gui_string(%s)" % n),
        "Atom": ("const char *%s" % n, "bmds_gui_atom_value(bmds_gui_atom(gui, %s))" % n),
        "Id": ("uint32_t %s" % n, "bmds_gui_id(%s)" % n),
        "Rect": ("int32_t %s_x, int32_t %s_y, uint32_t %s_width, uint32_t %s_height" % (n, n, n, n),
                 "bmds_gui_rect(%s_x, %s_y, %s_width, %s_height)" % (n, n, n, n)),
        "Color": ("uint32_t %s" % n, "bmds_gui_color(%s)" % n),
        "List": ("const char *const *%s, uint32_t %s_count" % (n, n), "bmds_gui_strings(gui, %s, %s_count)" % (n, n)),
    }[tag]


def parse(path):
    """The entries, in order: (kind, [fields])."""
    entries = []
    for number, line in enumerate(open(path, encoding="utf-8"), 1):
        m = ENTRY.match(line)
        if not m:
            continue
        fields = [f.strip() for f in m.group(2).split(",")]
        fields = [f for f in fields if f]
        kind = m.group(1)[4:].lower()
        entries.append((kind, fields, number))
    return entries


def lock_lines(entries):
    out = []
    for kind, f, _ in entries:
        if kind == "class":
            out.append("class %s : %s %s" % (f[0], f[1], f[2]))
        elif kind == "prop":
            out.append("property %s.%s %s %s" % (f[0], f[1], f[2], f[3]))
        elif kind == "values":
            out.append("values %s.%s %s" % (f[0], f[1], " ".join(f[2:])))
        elif kind == "method":
            out.append("method %s.%s(%s) -> %s" % (f[0], f[1], ", ".join(f[3:]), f[2]))
        elif kind == "event":
            out.append("event %s.%s(%s) %s" % (f[0], f[1], ", ".join(f[3:]), f[2].lower()))
    return out


def snake(name):
    return re.sub(r"(?<=[a-z0-9])([A-Z])", r"_\1", name).lower()


def header(entries):
    o = []
    o.append("/* bmds/guiclasses.h: the GUI classes' C wrappers (devplan/guiprograms.md §10). Generated")
    o.append("   by tools/guigen.py from sdk/gui/classes.def: don't edit; change the schema and run it. */")
    o.append("/* clang-format off */")
    o.append("")
    o.append("#ifndef BMDS_GUICLASSES_H")
    o.append("#define BMDS_GUICLASSES_H")
    o.append("")
    o.append("#include <bmds/gui.h>")
    o.append("")
    o.append("#ifdef __cplusplus")
    o.append('extern "C" {')
    o.append("#endif")
    events = []
    for kind, f, _ in entries:
        if kind == "class":
            c = f[0]
            o.append("")
            o.append("/* ---- %s%s */" % (c, " (of %s)" % f[1] if f[1] != "None" else ""))
            if f[2] == "Concrete":
                o.append("/** @brief A new %s under @p parent (0: none): its id, or 0. */" % c)
                o.append("static inline uint32_t bmds_%s_new(bmds_gui_t *gui, uint32_t parent) {" % snake(c))
                o.append('        return bmds_gui_new(gui, "%s", parent);' % c)
                o.append("}")
        elif kind == "prop":
            c, n, t, access = f
            if access in ("RW", "WO"):
                params, make = param(t, "value")
                o.append("static inline int bmds_%s_set_%s(bmds_gui_t *gui, uint32_t id, %s) {" % (snake(c), snake(n), params))
                o.append('        return bmds_gui_set_named(gui, id, "%s", %s);' % (n, make))
                o.append("}")
            if access in ("RW", "RO"):
                o.append("static inline int bmds_%s_get_%s(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {"
                         % (snake(c), snake(n)))
                o.append('        return bmds_gui_get_named(gui, id, "%s", out);' % n)
                o.append("}")
        elif kind == "method":
            c, n, result = f[0], f[1], f[2]
            args = f[3:]
            params = ["bmds_gui_t *gui", "uint32_t id"]
            makes = []
            for i, t in enumerate(args):
                p, m = param(t, "a%d" % i)
                params.append(p)
                makes.append(m)
            if result != "None":
                params.append("bmds_gui_value_t *result")
            o.append("static inline int bmds_%s_%s(%s) {" % (snake(c), snake(n), ", ".join(params)))
            if makes:
                o.append("        const bmds_gui_value_t args[] = {%s};" % ", ".join(makes))
            o.append('        return bmds_gui_call_named(gui, id, "%s", %s, %d, %s);'
                     % (n, "args" if makes else "NULL", len(makes), "result" if result != "None" else "NULL"))
            o.append("}")
        elif kind == "event":
            name = "BMDS_GUI_%s_%s" % (snake(f[0]).upper(), snake(f[1]).upper())
            o.append('#define %s "%s" /**< %s\'s event (%s). */' % (name, f[1], f[0], ", ".join(f[3:]) or "no values"))
    o.append("")
    o.append("#ifdef __cplusplus")
    o.append("}")
    o.append("#endif")
    o.append("")
    o.append("#endif")
    return "\n".join(o) + "\n"


def check(entries):
    """The schema's own rules: known tags, a class before its entries, parents known."""
    classes = set()
    problems = []
    for kind, f, number in entries:
        if kind == "class":
            if f[1] != "None" and f[1] not in classes:
                problems.append("%d: %s's parent %s isn't declared before it" % (number, f[0], f[1]))
            classes.add(f[0])
            continue
        if f[0] not in classes:
            problems.append("%d: %s isn't a class (yet)" % (number, f[0]))
        tags = []
        if kind == "prop":
            tags = [f[2]]
        elif kind == "method":
            tags = f[2:]
        elif kind == "event":
            tags = f[3:]
        for t in tags:
            if t != "None" and t not in TAGS:
                problems.append("%d: no tag %s" % (number, t))
    return problems


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("header", "lock"):
        print(__doc__ or "usage: guigen.py header|lock CLASSES.DEF [OUT]", file=sys.stderr)
        return 2
    entries = parse(sys.argv[2])
    problems = check(entries)
    for p in problems:
        print("classes.def:%s" % p, file=sys.stderr)
    if problems:
        return 1
    if sys.argv[1] == "lock":
        print("\n".join(lock_lines(entries)))
        return 0
    text = header(entries)
    names = re.findall(r"^static inline \w+ (\w+)\(", text, re.M)
    twice = sorted({n for n in names if names.count(n) > 1})
    if twice:
        print("classes.def: names made twice: %s" % ", ".join(twice), file=sys.stderr)
        return 1
    with open(sys.argv[3], "w", encoding="utf-8") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
