#!/usr/bin/env python3
# The script API's lock (devplan/scripting.md §7): every declaration in api.lock must still be
# in `angel --api`, unchanged (the API is userspace ABI: append only). A declaration angel has
# that the lock doesn't is new API: append it to the lock (the one edit to it that is allowed).
#
#   apilock.py ANGEL LOCK

import subprocess
import sys


def main():
    current = set(subprocess.run([sys.argv[1], "--api"], capture_output=True, check=True, text=True).stdout.splitlines())
    locked = [line for line in open(sys.argv[2], encoding="utf-8").read().splitlines()
              if line and not line.startswith("#")]
    missing = [line for line in locked if line not in current]
    new = sorted(current - set(locked))
    for line in missing:
        print("removed or changed (userspace ABI!): %s" % line)
    for line in new:
        print("new, not in the lock (append it): %s" % line)
    return 1 if missing or new else 0


if __name__ == "__main__":
    sys.exit(main())
