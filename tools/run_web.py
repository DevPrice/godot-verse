#!/usr/bin/env python3
"""Serve a directory locally and watch headless Chrome's console output.

Serves `directory` over http://127.0.0.1:<port> with Python's own
http.server (threaded, in-process — no subprocess, no third-party
dependency), launches headless Chrome against it, and streams the page's
console messages line by line. Console text reaches this process through
Chrome's own `--enable-logging=stderr`, which tags each console call as
"...:CONSOLE:<line>] <text>" on stderr — no DevTools-protocol websocket
client needed. Exits 0 the moment a line matches --until, non-zero on
--timeout or if Chrome exits first.

Usage:
    python tools/run_web.py <directory> --until SENTINEL --timeout 30
    python tools/run_web.py <directory> --until SENTINEL --timeout 30 --coop-coep
"""

import argparse
import functools
import http.server
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

DEFAULT_CHROME = "C:/Program Files/Google/Chrome/Application/chrome.exe"
DEFAULT_USER_DATA_ROOT = "C:/Users/Devin/AppData/Local/Temp/verse-web-chrome"

CONSOLE_LINE = re.compile(r":(?:INFO|WARNING|ERROR|VERBOSE\d*):CONSOLE:\d+\] (.*)$")


def make_handler(directory, coop_coep):
    class Handler(http.server.SimpleHTTPRequestHandler):
        def end_headers(self):
            if coop_coep:
                self.send_header("Cross-Origin-Opener-Policy", "same-origin")
                self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
            super().end_headers()

        def log_message(self, format, *args):
            pass

    return functools.partial(Handler, directory=directory)


def start_server(directory, port, coop_coep):
    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", port), make_handler(directory, coop_coep)
    )
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


def start_chrome(url, chrome_path, user_data_dir, extra_args):
    os.makedirs(user_data_dir, exist_ok=True)
    args = [
        chrome_path,
        "--headless=new",
        "--disable-gpu",
        "--no-sandbox",
        "--user-data-dir=" + user_data_dir,
        "--enable-logging=stderr",
    ] + list(extra_args) + [url]
    return subprocess.Popen(
        args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1
    )


def stop_by_pid(pid):
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(pid), "/T", "/F"],
            capture_output=True,
        )
        return
    try:
        os.kill(pid, 15)
    except OSError:
        return
    try:
        os.waitpid(pid, 0)
    except OSError:
        pass


def pump_lines(pipe, q):
    for line in iter(pipe.readline, ""):
        q.put(line)
    q.put(None)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", help="directory to serve at /")
    parser.add_argument("--page", default="index.html", help="page to open, relative to directory")
    parser.add_argument("--port", type=int, default=0, help="0 picks a free port")
    parser.add_argument("--coop-coep", action="store_true", help="serve COOP/COEP headers")
    parser.add_argument("--until", required=True, help="regex; a matching console line exits 0")
    parser.add_argument("--timeout", type=float, default=30.0, help="seconds to wait for --until")
    parser.add_argument("--chrome", default=DEFAULT_CHROME, help="path to chrome.exe")
    parser.add_argument(
        "--user-data-root", default=DEFAULT_USER_DATA_ROOT, help="parent dir for a fresh --user-data-dir"
    )
    parser.add_argument(
        "--chrome-arg", action="append", default=[], help="extra chrome flag, may repeat"
    )
    parser.add_argument(
        "--verbose", action="store_true", help="also print chrome's non-console log lines, to stderr"
    )
    args = parser.parse_args(argv)

    directory = os.path.abspath(args.directory)
    if not os.path.isdir(directory):
        print(f"run_web.py: no such directory: {directory}", file=sys.stderr)
        return 2
    if not os.path.isfile(args.chrome):
        print(f"run_web.py: no chrome at {args.chrome}", file=sys.stderr)
        return 2

    server, server_thread = start_server(directory, args.port, args.coop_coep)
    port = server.server_address[1]
    url = f"http://127.0.0.1:{port}/{args.page}"

    os.makedirs(args.user_data_root, exist_ok=True)
    user_data_dir = tempfile.mkdtemp(prefix="run-", dir=args.user_data_root)

    until_re = re.compile(args.until)
    proc = None
    matched = False
    try:
        proc = start_chrome(url, args.chrome, user_data_dir, args.chrome_arg)
        q = queue.Queue()
        reader = threading.Thread(target=pump_lines, args=(proc.stdout, q), daemon=True)
        reader.start()

        deadline = time.time() + args.timeout
        chrome_exited = False
        while time.time() < deadline:
            remaining = deadline - time.time()
            try:
                line = q.get(timeout=min(remaining, 0.5))
            except queue.Empty:
                continue
            if line is None:
                chrome_exited = True
                break
            line = line.rstrip("\n")
            console_match = CONSOLE_LINE.search(line)
            if console_match:
                text = console_match.group(1)
                print(text)
                if until_re.search(text):
                    matched = True
                    break
            elif args.verbose:
                print(line, file=sys.stderr)

        if not matched:
            if chrome_exited:
                print("run_web.py: chrome exited before the pattern matched", file=sys.stderr)
            else:
                print(f"run_web.py: timed out after {args.timeout}s waiting for {args.until!r}", file=sys.stderr)
    finally:
        if proc is not None and proc.poll() is None:
            stop_by_pid(proc.pid)
        server.shutdown()
        server_thread.join(timeout=5)
        shutil.rmtree(user_data_dir, ignore_errors=True)

    return 0 if matched else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
