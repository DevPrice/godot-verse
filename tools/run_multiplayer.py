#!/usr/bin/env python3
"""R-EXP-9's second peer: two headless games of tests/integration exchanging a Verse `@rpc` call.

docs/editor-test-audit.md step 8, and by-hand-findings.md's "R-EXP-9's other half": a single
process can see that `Script.get_rpc_config()` is what Godot reads, and cannot see a call arrive.
This starts `multiplayer/peer.tscn` twice -- a host and a client over ENet on localhost -- and the
host walks the six by-hand steps, printing one `[multiplayer] <case>: ok|FAIL` line each. What only
the printed output carries, Godot's own refusal sentences at each end, is checked here, and then
the summary line run_tests.py requires.

    python tools/run_multiplayer.py --godot <godot> --project tests/integration

Each game's output goes to a file rather than a pipe (a game blocks before its first frame on a pipe
nobody drains) and each gets its own --log-file, because both share the project's user://.
"""

import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import godot_env  # noqa: E402

TAG = "[multiplayer]"
SCENE = "res://multiplayer/peer.tscn"
# Each game compiles the whole project before its first frame, so the host has to be up before the
# client is started and both are given the time a cold build takes.
LISTEN_TIMEOUT = 180
RUN_TIMEOUT = 300


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def start(godot: str, project: Path, role: str, port: int, out: Path) -> "tuple[subprocess.Popen, object]":
    log = open(out, "w", encoding="utf-8", errors="replace")
    game = subprocess.Popen(
        [godot, "--headless", "--path", str(project), "--log-file", str(out.with_suffix(".godot.log")),
         "--scene", SCENE, "--", f"--mp-role={role}", f"--mp-port={port}"],
        stdout=log, stderr=subprocess.STDOUT, env=godot_env.env_for(godot_env.DEFAULT_HOME))
    return game, log


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--godot", required=True)
    parser.add_argument("--project", required=True, help="tests/integration, with the extension staged")
    args = parser.parse_args()
    project = Path(args.project)
    logs = Path(__file__).resolve().parent.parent / "bin"
    logs.mkdir(parents=True, exist_ok=True)
    server_out = logs / "multiplayer_server.log"
    client_out = logs / "multiplayer_client.log"
    port = free_udp_port()

    passed = failed = 0

    def check(name: str, ok: bool, detail: str = "") -> None:
        nonlocal passed, failed
        if ok:
            passed += 1
            print(f"{TAG} {name}: ok", flush=True)
        else:
            failed += 1
            print(f"{TAG} {name}: FAIL" + (f" ({detail})" if detail else ""), flush=True)

    server, server_log = start(args.godot, project, "server", port, server_out)
    client = client_log = None
    try:
        deadline = time.monotonic() + LISTEN_TIMEOUT
        while time.monotonic() < deadline and server.poll() is None and "[mp-server] listening" not in read(server_out):
            time.sleep(0.25)
        listening = "[mp-server] listening" in read(server_out)
        check("(1) the host's ENet server listens", listening)
        if listening:
            client, client_log = start(args.godot, project, "client", port, client_out)
            for game in (server, client):
                try:
                    game.wait(RUN_TIMEOUT)
                except subprocess.TimeoutExpired:
                    pass
    finally:
        for game in (server, client):
            if game is not None and game.poll() is None:
                game.kill()
                game.wait(30)
        for log in (server_log, client_log):
            if log is not None:
                log.close()

    server_text = read(server_out)
    client_text = read(client_out)
    for line in server_text.splitlines():
        if line.startswith(TAG):
            print(line, flush=True)
            if line.endswith(": ok"):
                passed += 1
            elif ": FAIL" in line:
                failed += 1
    if listening:
        check("(2) the client connects to the host", "[mp-client] connected" in client_text)
        check("the host walked every step and left", server.returncode == 0 and f"{TAG} (6) and sends nothing for it: " in server_text,
              f"exit {server.returncode}")
        check("and the client left when told", client is not None and client.returncode == 0,
              f"exit {client.returncode if client is not None else None}")
        # SceneRPCInterface::_process_rpc and ::rpcp, verbatim but for the peer id.
        check("(3) Godot's refusal at the host names the mode and the authority",
              "RPC 'TakeDamage' is not allowed on node /root/Peer/Rpcs from: " in server_text
              and 'Mode is "authority", authority is 1.' in server_text)
        for method in ("Ordinary", "Misspelled"):
            check(f"(6) Godot's refusal at the client says {method} is not marked for RPCs",
                  f'Unable to get the RPC configuration for the function "{method}" at path: "/root/Peer/Rpcs". '
                  "This happens when the method is missing or not marked for RPCs in the local script." in client_text)
    # Under a tag of its own, so an echoed case line is not read as a second case.
    if failed:
        for name, text in (("host", server_text), ("client", client_text)):
            for line in [line for line in text.splitlines() if line.strip()][-20:]:
                print(f"[multiplayer-log] {name}: {line}")
    print(f"{TAG} {passed} passed, {failed} failed, 0 skipped", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
