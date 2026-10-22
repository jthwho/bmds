#!/usr/bin/env python3
# The script corpus (devplan/scripting.md §8): runs every tests/scripts/*.as with angel and
# compares its stdout with NAME.out, its stderr with NAME.err (none: nothing on stderr) and its
# exit status with the script's `// exit: N` line (default 0). A `// args: ...` line gives the
# arguments, `// stdin: ...` the input (\n for newlines). Scripts run from this directory; a
# temporary directory for their files is in $TEST_TMP, removed after. $TEST_HTTP is the address
# of a small HTTP server on loopback (http_server below) for the network scripts.
#
#   run.py ANGEL [DIR]

import http.server
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    # What the network scripts ask for: a plain page, a chunked one, redirects, a missing page
    # and an echo of the request.
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, status, body, headers=()):
        data = body.encode()
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def answer(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length).decode() if length else ""
        if self.path == "/hello":
            self.reply(200, "Hello, world!\n", [("Content-Type", "text/plain"), ("X-Test", "yes")])
        elif self.path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for part in ("first ", "second ", "third"):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part.encode()))
            self.wfile.write(b"0\r\n\r\n")
        elif self.path == "/close":
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"until the end")
            self.close_connection = True
        elif self.path == "/moved":
            self.reply(301, "", [("Location", "/hello")])
        elif self.path == "/form":
            self.reply(303, "", [("Location", "echo")])
        elif self.path == "/loop":
            self.reply(302, "", [("Location", "/loop")])
        else:
            echo = "%s %s %s [%s] %s" % (self.command, self.path, self.headers.get("Content-Type"),
                                         self.headers.get("X-Name"), body)
            self.reply(200 if self.path.startswith("/echo") else 404, echo)

    do_GET = do_POST = do_PUT = do_HEAD = do_DELETE = answer


def http_server():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def header(text, key):
    m = re.search(r"^// " + key + r": ?(.*)$", text, re.M)
    return m.group(1) if m else None


def main():
    angel = os.path.abspath(sys.argv[1])
    directory = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.abspath(__file__))
    failures = 0
    server = http_server()
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
            env = dict(os.environ, TEST_TMP=scratch, TEST_HTTP="http://127.0.0.1:%d" % server.server_port)
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
    server.shutdown()
    print("%d scripts, %d failed" % (len(names), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
