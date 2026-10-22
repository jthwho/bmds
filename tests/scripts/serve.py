#!/usr/bin/env python3
# angel --serve and --report-fd (devplan/editor.md §9): the editor's checker and Run's report.
# The protocol is ABI once shipped: these are its checks.
#
#   serve.py ANGEL

import os
import shutil
import subprocess
import sys
import tempfile

failures = 0


def check(what, ok):
    global failures
    if not ok:
        failures += 1
        print("FAIL", what)


# Runs the script with a pipe on descriptor 3 (the shell puts it there): its result and the report.
def reported(angel, script):
    r, w = os.pipe()
    result = subprocess.run(["sh", "-c", 'exec "$0" --report-fd 3 "$1" 3>&%d' % w, angel, script], pass_fds=(w,),
                            capture_output=True)
    os.close(w)
    report = os.read(r, 4096).decode()
    os.close(r)
    return result, report


def main():
    angel = os.path.abspath(sys.argv[1])
    scratch = tempfile.mkdtemp(prefix="angel-serve-")
    try:
        lib = os.path.join(scratch, "lib.as")
        with open(lib, "w") as f:
            f.write("int twice(int n) { return n * 2; }\n")
        p = subprocess.Popen([angel, "--serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

        def ask(header, body=b""):
            p.stdin.write(header.encode() + b"\n" + body)
            p.stdin.flush()
            lines = []
            while True:
                line = p.stdout.readline().decode()
                if line in ("end\n", ""):
                    return lines
                lines.append(line.rstrip("\n").split("\t"))

        def check_text(text):
            body = text.encode()
            return ask("check\t%s\t%d" % (os.path.join(scratch, "main.as"), len(body)), body)

        api = ask("api")
        kinds = {r[1] for r in api}
        check("api: every kind", {"type", "method", "function", "constant", "variable", "property"} <= kinds)
        check("api: a method with its help",
              ["api", "method", "Window.fillRect", "void Window::fillRect(int, int, int, int, uint)",
               "A filled rectangle."] in api)
        check("api: a namespaced function", any(r[2] == "Http::get" for r in api))

        r = check_text("int x = 1;\nvoid f() { int y = \"s\"; }\nprintln(z);\n")
        diags = [x for x in r if x[0] == "diag"]
        check("check: two errors", len(diags) == 2)
        check("check: where", diags and diags[0][3:5] == ["2", "20"] and diags[1][3:5] == ["3", "9"])
        check("check: no symbols from a failed build", not [x for x in r if x[0] == "symbol"])

        r = check_text('#include "lib.as"\nclass Ball { float x; void move() {} }\nWindow w("t", 10, 10);\n'
                       "println(twice(2));\n")
        check("check: clean", not [x for x in r if x[0] == "diag"])
        names = {(x[1], x[2]) for x in r if x[0] == "symbol"}
        check("check: symbols", {("class", "Ball"), ("method", "Ball.move"), ("property", "Ball.x"),
                                 ("variable", "w"), ("function", "twice")} <= names)
        check("check: a function's file and line",
              any(x[2] == "twice" and x[4].endswith("lib.as") and x[5] == "1" for x in r if x[0] == "symbol"))

        r = check_text("void main() {}\nprintln(1);\n")
        check("check: main and statements", any("can't have a main()" in x[5] for x in r if x[0] == "diag"))
        r = check_text('#include "missing.as"\n')
        check("check: a missing include", any("can't include" in x[5] for x in r if x[0] == "diag"))
        check("unknown request", ask("bogus") == [["error", "unknown request"]])
        p.stdin.write(b"quit\n")
        p.stdin.flush()
        check("quit", p.wait(timeout=10) == 0)

        # --report-fd: an uncaught exception, and a compile error, as lines on descriptor 3.
        script = os.path.join(scratch, "fails.as")
        with open(script, "w") as f:
            f.write('println("a");\nint[] a;\na[3] = 1;\n')
        result, report = reported(angel, script)
        check("report-fd: exit status", result.returncode == 1)
        check("report-fd: the exception", report == "exception\t%s\t3\t1\tIndex out of bounds\n" % script)
        with open(script, "w") as f:
            f.write("int x = ;\n")
        result, report = reported(angel, script)
        check("report-fd: a compile error", report.startswith("error\t%s\t1\t" % script))
        check("report-fd: bad descriptor", subprocess.run([angel, "--report-fd", "x", script],
                                                         capture_output=True).returncode == 2)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print("serve: %d failed" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
