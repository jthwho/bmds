#!/usr/bin/env python3
"""Checks the shell corpora (syntax.txt, run.txt) against real bash.

Every `ok` entry must pass `bash -n`; every `error` entry must fail it on the
same line with the same message. `incomplete` and `unsupported` entries are
not checked (bash -n reads a whole file, so it has no "need more input").
Every run.txt script must print the same stdout and exit with the same status
under `bash -c`.
"""
import os
import pathlib
import subprocess
import sys
import tempfile


def entries(path):
    return list(_entries(path))  # fully read: entries are filled in after they start


def _entries(path):
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


def run_entries(path):
    # Fully read (entries are filled in after they start); blank lines at the
    # end of an output block are the separator, not output.
    entries = list(_run_entries(path))
    for e in entries:
        while e["out"].endswith("\n\n") or e["out"] == "\n":
            e["out"] = e["out"][:-1]
    return entries


def _run_entries(path):
    entry = None
    state = None
    for line in path.read_text().splitlines():
        if line.startswith("=== "):
            entry = {"name": line[4:], "script": "", "out": "", "status": 0}
            state = "script"
            yield entry
        elif entry is None:
            continue
        elif line == "--- out":
            state = "out"
        elif state == "out" and line.startswith("--- status "):
            entry["status"] = int(line[11:])
            state = None
        elif state == "script":
            entry["script"] += line + "\n"
        elif state == "out":
            entry["out"] += line + "\n"


def check_runs(corpus_dir):
    failures = 0
    # The C locale: globs sort bytewise (the corpus uses names whose order is
    # the same as BMDS's case-insensitive one).
    env = dict(os.environ, LC_ALL="C")
    for e in run_entries(corpus_dir / "run.txt"):
        # Each script starts in an empty directory of its own, as BMDS's start
        # on a freshly formatted RAM disk.
        with tempfile.TemporaryDirectory() as empty:
            # lastpipe: the last pipeline stage runs in the shell, as in BMDS.
            r = subprocess.run(["bash", "--norc", "--noprofile", "-O", "lastpipe", "-c", e["script"]], cwd=empty,
                               text=True, capture_output=True, env=env)
            if r.stdout != e["out"] or r.returncode != e["status"]:
                failures += 1
                print(f"FAIL run '{e['name']}': bash printed {r.stdout!r} with status {r.returncode}")
    return failures


def main():
    corpus_dir = pathlib.Path(sys.argv[1])
    corpus = corpus_dir / "syntax.txt"
    failures = check_runs(corpus_dir)
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
