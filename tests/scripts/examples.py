#!/usr/bin/env python3
# The examples and the guide's lessons (devplan/scripting.md §14): every one builds with
# angel -c (an API change that breaks one fails here), and the lessons that only print run and
# print what tests/scripts/guide/NAME.out says.
#
#   examples.py ANGEL ROOT

import os
import subprocess
import sys


def main():
    angel = os.path.abspath(sys.argv[1])
    root = sys.argv[2]
    failures = 0
    folders = [os.path.join(root, "programs/angel/examples"), os.path.join(root, "programs/angel/guide")]
    scripts = sorted(os.path.join(f, n) for f in folders for n in os.listdir(f) if n.endswith(".as"))
    for script in scripts:
        r = subprocess.run([angel, "-c", script], capture_output=True, text=True)
        if r.returncode != 0 or r.stdout or r.stderr:
            failures += 1
            print("FAIL %s:\n%s%s" % (script, r.stdout, r.stderr))
    expected_dir = os.path.join(root, "tests/scripts/guide")
    for name in sorted(os.listdir(expected_dir)):
        lesson = os.path.join(root, "programs/angel/guide", name[:-len(".out")] + ".as")
        with open(os.path.join(expected_dir, name)) as f:
            expected = f.read()
        r = subprocess.run([angel, lesson], stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
        if r.returncode != 0 or r.stdout != expected:
            failures += 1
            print("FAIL %s: status %d\n--- got\n%s--- expected\n%s" % (lesson, r.returncode, r.stdout, expected))
    print("%d scripts, %d failed" % (len(scripts), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
