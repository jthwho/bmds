#!/usr/bin/env python3
# angel --serve, --report-fd and --debug (devplan/editor.md §9, §17): the editor's checker, Run's
# report and the debugger. The protocols are ABI once shipped: these are their checks.
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


# Runs the script under the debugger: commands on descriptor 4, events on 3 (the shell puts them
# there). Returns the process, the events' reader and a function sending a command.
def debugged(angel, script):
    events_r, events_w = os.pipe()
    commands_r, commands_w = os.pipe()
    p = subprocess.Popen(["sh", "-c", 'exec "$0" --report-fd 3 --debug 4 "$1" 3>&%d 4<&%d' % (events_w, commands_r),
                          angel, script], pass_fds=(events_w, commands_r), stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    os.close(events_w)
    os.close(commands_r)
    events = os.fdopen(events_r)
    commands = os.fdopen(commands_w, "w")

    def send(line):
        commands.write(line + "\n")
        commands.flush()

    return p, events, commands, send


def answer(events):
    lines = []
    while True:
        line = events.readline()
        if line in ("end\n", ""):
            return lines
        lines.append(line.rstrip("\n").split("\t"))


def debug_checks(angel, scratch):
    script = os.path.join(scratch, "debug.as")
    with open(script, "w") as f:
        f.write("class Ball { float x = 1.5; int[] hits = {1, 2, 3}; string name = \"a\\tb\"; }\n"  # 1
                "int twice(int n) {\n"                                                          # 2
                "        int r = n * 2;\n"                                                      # 3
                "        return r;\n"                                                           # 4
                "}\n"                                                                           # 5
                "Ball b;\n"                                                                     # 6
                "int total = 0;\n"                                                              # 7
                "for (int i = 0; i < 3; i++) {\n"                                               # 8
                "        total += twice(i);\n"                                                  # 9
                "}\n"                                                                           # 10
                "println(total);\n"                                                             # 11
                "int[] a;\n"                                                                    # 12
                "a[2] = 1;\n")                                                                  # 13
    p, events, commands, send = debugged(angel, script)
    send("break\t%s\t3" % script)
    send("go")
    check("debug: a breakpoint", events.readline() == "stopped\tbreak\t%s\t3\n" % script)
    send("stack")
    check("debug: the stack", answer(events) == [["frame", "0", "int twice(int)", script, "3"],
                                                 ["frame", "1", "the top level", script, "9"]])
    send("vars\t0\t")
    check("debug: a frame's variables", answer(events) == [["var", "n", "int", "0", "0"]])
    send("vars\tg\t")
    check("debug: the globals", answer(events) == [["var", "total", "int", "0", "0"],
                                                   ["var", "b", "Ball", '{x: 1.5, hits: […], name: "a\\tb"}', "3"],
                                                   ["var", "a", "int[]", "(not set yet)", "0"]])  # line 12: not reached
    send("vars\tg\tb")
    check("debug: an object's members", answer(events) == [["var", "x", "float", "1.5", "0"],
                                                           ["var", "hits", "int[]", "[1, 2, 3]", "3"],
                                                           ["var", "name", "string", '"a\\tb"', "0"]])
    send("vars\t1\tb.hits")
    check("debug: an array's elements", [v[1] for v in answer(events)] == ["[0]", "[1]", "[2]"])
    send("value\t0\tb.hits[1]")
    check("debug: a path's value", answer(events) == [["var", "b.hits[1]", "int", "2", "0"]])
    send("value\t0\tnope")
    check("debug: no such variable", answer(events) == [["error", "no such variable here"]])
    send("value\t0\tb.hits[9]")
    check("debug: an index too far", answer(events) == [["error", "index out of bounds"]])
    send("bogus")
    check("debug: an unknown command", answer(events) == [["error", "unknown command"]])
    send("next")
    check("debug: step over", events.readline() == "stopped\tstep\t%s\t4\n" % script)
    send("out")
    check("debug: step out", events.readline().startswith("stopped\tstep\t%s\t" % script))
    send("vars\tg\t")
    check("debug: a value changed", ["var", "total", "int", "0", "0"] in answer(events))
    send("clear\t%s\t3" % script)
    send("go")
    check("debug: an uncaught exception",
          events.readline() == "stopped\texception\t%s\t13\tIndex out of bounds\n" % script)
    send("go")
    check("debug: then the report", events.readline() == "exception\t%s\t13\t1\tIndex out of bounds\n" % script)
    check("debug: the exit status", p.wait(timeout=10) == 1)
    check("debug: the output", p.stdout.read() == b"6\n")
    events.close()
    commands.close()

    # Step from the first line; a pause while it runs; the editor gone: it runs on, undebugged.
    loop = os.path.join(scratch, "loop.as")
    with open(loop, "w") as f:
        f.write("int n = 0;\nwhile (n >= 0) {\n        n++;\n        if (n == 100000000) n = 0;\n}\n")
    p, events, commands, send = debugged(angel, loop)
    send("step")
    check("debug: the first line", events.readline() == "stopped\tstep\t%s\t1\n" % loop)
    send("go")
    send("pause")
    check("debug: a pause", events.readline().startswith("stopped\tpause\t%s\t" % loop))
    commands.close()
    try:
        p.wait(timeout=0.5)
        check("debug: runs on when the editor goes", False)
    except subprocess.TimeoutExpired:
        pass
    p.kill()
    p.wait()
    events.close()
    check("debug: --debug needs --report-fd",
          subprocess.run([angel, "--debug", "4", loop], capture_output=True).returncode == 2)


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
        debug_checks(angel, scratch)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print("serve: %d failed" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
