#!/usr/bin/env python3
# A deck's script runs under angel unchanged (devplan/preso.md §8c): every declaration in
# `preso --api` (the deck profile) must be in `angel --api` too, the same.
#
#   subset.py ANGEL PRESO

import subprocess
import sys


def api(program):
    return set(subprocess.run([program, "--api"], capture_output=True, check=True, text=True).stdout.splitlines())


def main():
    missing = sorted(api(sys.argv[2]) - api(sys.argv[1]))
    for line in missing:
        print("a deck has it, angel hasn't: %s" % line)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
