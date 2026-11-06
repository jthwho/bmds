#!/usr/bin/env python3
# The example decks' scripts run under angel as they do in their decks (devplan/preso.md §8c):
# with no screen, the stage plays each to its end, its beats taken at once. Each must end
# well and say nothing on standard error.
#
#   underangel.py ANGEL ROOT

import glob
import os
import subprocess
import sys


def main():
    angel, root = sys.argv[1], sys.argv[2]
    scripts = sorted(glob.glob(os.path.join(root, "programs/preso/examples/**/*.as"), recursive=True))
    failed = 0
    for script in scripts:
        r = subprocess.run([angel, script], capture_output=True, text=True, timeout=60,
                           cwd=os.path.dirname(script))
        if r.returncode != 0 or r.stderr:
            failed += 1
            print("%s: exit %d\n%s" % (os.path.relpath(script, root), r.returncode, r.stderr))
    if not scripts:
        print("no scripts found")
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
