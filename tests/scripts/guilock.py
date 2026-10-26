#!/usr/bin/env python3
# The GUI classes' lock (devplan/guiprograms.md §10): every line of gui.lock must still be in the
# schema (sdk/gui/classes.def), unchanged: the classes are userspace ABI, append only. A line
# the schema has that the lock doesn't is new ABI: append it to the lock (the one edit allowed).
# And bmds/guiclasses.h and docs/gui-classes.md must be what the generator writes from the schema.
#
#   guilock.py ROOT

import os
import subprocess
import sys
import tempfile


def main():
    root = sys.argv[1]
    gen = os.path.join(root, "tools/guigen.py")
    schema = os.path.join(root, "sdk/gui/classes.def")
    current = set(subprocess.run([sys.executable, gen, "lock", schema], capture_output=True, check=True,
                                 text=True).stdout.splitlines())
    locked = [line for line in open(os.path.join(root, "tests/scripts/gui.lock"), encoding="utf-8").read().splitlines()
              if line and not line.startswith("#")]
    failures = 0
    for line in locked:
        if line not in current:
            print("removed or changed (userspace ABI!): %s" % line)
            failures += 1
    for line in sorted(current - set(locked)):
        print("new, not in the lock (append it): %s" % line)
        failures += 1
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as scratch:
        out = os.path.join(scratch, "guiclasses.h")
        subprocess.run([sys.executable, gen, "header", schema, out], check=True)
        if open(out, encoding="utf-8").read() != open(os.path.join(root, "sdk/include/bmds/guiclasses.h"),
                                                      encoding="utf-8").read():
            print("sdk/include/bmds/guiclasses.h is out of date: run tools/guigen.py header")
            failures += 1
        out = os.path.join(scratch, "gui-classes.md")
        subprocess.run([sys.executable, gen, "doc", schema, out], check=True)
        if open(out, encoding="utf-8").read() != open(os.path.join(root, "docs/gui-classes.md"),
                                                      encoding="utf-8").read():
            print("docs/gui-classes.md is out of date: run tools/guigen.py doc")
            failures += 1
    print("gui lock: %d problem(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
