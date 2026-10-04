#!/usr/bin/env python3
"""Checks tests/host/corpus/syntax.txt against real bash.

Every `ok` entry must pass `bash -n`; every `error` entry must fail it on the
same line with the same message. `incomplete` and `unsupported` entries are
not checked (bash -n reads a whole file, so it has no "need more input").
"""
import pathlib
import subprocess
import sys


def entries(path):
    entry = None
    for line in path.read_text().splitlines():
        if line.startswith("=== "):
            entry = {"name": line[4:], "input": "", "outcome": None}
            yield entry
        elif entry is not None and entry["outcome"] is None:
            if line.startswith("--- "):
                rest = line[4:]
                entry["outcome"] = rest.split(" ", 1)[0]
                if entry["outcome"] in ("error", "unsupported"):
                    where, message = rest.split(" ", 1)[1].split(": ", 1)
                    entry["line"] = int(where)
                    entry["message"] = message
            else:
                entry["input"] += line + "\n"


def main():
    corpus = pathlib.Path(sys.argv[1]) / "syntax.txt"
    failures = 0
    for e in entries(corpus):
        if e["outcome"] not in ("ok", "error"):
            continue
        r = subprocess.run(["bash", "-n"], input=e["input"], text=True, capture_output=True)
        if e["outcome"] == "ok" and r.returncode != 0:
            failures += 1
            print(f"FAIL {e['name']}: bash rejects it: {r.stderr.strip()}")
        elif e["outcome"] == "error":
            want = f"line {e['line']}: {e['message']}"
            if r.returncode == 0 or want not in r.stderr:
                failures += 1
                print(f"FAIL {e['name']}: expected '{want}', bash said: {r.stderr.strip() or 'ok'}")
    print(f"{failures} disagreement(s) with bash")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
