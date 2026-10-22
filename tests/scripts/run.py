#!/usr/bin/env python3
# The script corpus (devplan/scripting.md §8): runs every tests/scripts/*.as with angel and
# compares its stdout with NAME.out, its stderr with NAME.err (none: nothing on stderr) and its
# exit status with the script's `// exit: N` line (default 0). A `// args: ...` line gives the
# arguments, `// stdin: ...` the input (\n for newlines). Scripts run from this directory; a
# temporary directory for their files is in $TEST_TMP, removed after.
#
#   run.py ANGEL [DIR]

import os
import re
import shutil
import subprocess
import sys
import tempfile


def header(text, key):
    m = re.search(r"^// " + key + r": ?(.*)$", text, re.M)
    return m.group(1) if m else None


def main():
    angel = os.path.abspath(sys.argv[1])
    directory = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.abspath(__file__))
    failures = 0
    names = sorted(n for n in os.listdir(directory) if n.endswith(".as"))
    for name in names:
        base = name[:-3]
        text = open(os.path.join(directory, name), encoding="utf-8").read()
        args = (header(text, "args") or "").split()
        stdin = (header(text, "stdin") or "").replace("\\n", "\n")
        status = int(header(text, "exit") or "0")
        expected_out = open(os.path.join(directory, base + ".out"), encoding="utf-8").read()
        err_path = os.path.join(directory, base + ".err")
        expected_err = open(err_path, encoding="utf-8").read() if os.path.exists(err_path) else ""
        scratch = tempfile.mkdtemp(prefix="angel-")
        try:
            env = dict(os.environ, TEST_TMP=scratch)
            result = subprocess.run([angel, name] + args, cwd=directory, input=stdin.encode(), capture_output=True,
                                    timeout=30, env=env)
        finally:
            shutil.rmtree(scratch, ignore_errors=True)
        out, err = result.stdout.decode(errors="replace"), result.stderr.decode(errors="replace")
        problems = []
        if out != expected_out:
            problems.append("stdout:\n--- expected\n%s--- got\n%s" % (expected_out, out))
        if err != expected_err:
            problems.append("stderr:\n--- expected\n%s--- got\n%s" % (expected_err, err))
        if result.returncode != status:
            problems.append("exit status %d, expected %d" % (result.returncode, status))
        if problems:
            failures += 1
            print("FAIL %s\n%s" % (name, "\n".join(problems)))
    print("%d scripts, %d failed" % (len(names), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
