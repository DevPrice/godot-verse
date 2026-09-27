#!/usr/bin/env python3
"""Godot's remote-debug protocol from the editor's side, and the debug-wire layer's cases over it.

docs/editor-test-audit.md step 5's wire half: a headless game of tests/integration is launched with
`--remote-debug` pointed at a server here, which drives a breakpoint, the stack, Stack Variables,
Step Over/Into/Out, a breakpoint armed while the game runs, Skip Breakpoints and the script profiler
over the wire -- no editor at all. The editor layer asserts the same things through Godot's own
Debugger panel; this is the half that does not walk editor internals, so a panel that moves on a
Godot bump fails there and not here.

    python tools/debug_wire.py --godot <godot> --project tests/integration

`run_tests.py --only debug-wire` stages the extension into the project first, as the integration
layer does. One `[debug-wire] <case>: ok|FAIL|skip -- ...` line per case, then a summary line; exits
non-zero when a case failed. The game's own output goes to bin/debug_wire_game.log, and its last
lines are echoed when a case fails.

The protocol, as read from 4.7.2 (core/debugger/remote_debugger*.cpp, core/io/marshalls.cpp):
every message is a little-endian uint32 length and an encode_variant()'d Array of
[name, thread_id, data]. The game drops a message that is not exactly those three, or that is
addressed to a thread it does not know, without a word -- so a two-element message is silently
ignored rather than refused.
"""

from __future__ import annotations

import argparse
import socket
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import godot_env  # noqa: E402

TAG = "debug-wire"
MAIN_THREAD = 1  # Thread::MAIN_ID

NIL, BOOL, INT, FLOAT, STRING = 0, 1, 2, 3, 4
(VECTOR2, VECTOR2I, RECT2, RECT2I, VECTOR3, VECTOR3I, TRANSFORM2D, VECTOR4, VECTOR4I, PLANE,
 QUATERNION, AABB, BASIS, TRANSFORM3D, PROJECTION, COLOR) = range(5, 21)
STRING_NAME, NODE_PATH, RID, OBJECT, CALLABLE, SIGNAL, DICTIONARY, ARRAY = range(21, 29)
(PACKED_BYTE, PACKED_INT32, PACKED_INT64, PACKED_FLOAT32, PACKED_FLOAT64, PACKED_STRING,
 PACKED_VECTOR2, PACKED_VECTOR3, PACKED_COLOR, PACKED_VECTOR4) = range(29, 39)

FLAG_64 = 1 << 16
# The real_t math types by how many components they carry; a 64 flag means doubles.
_REALS = {VECTOR2: 2, RECT2: 4, VECTOR3: 3, TRANSFORM2D: 6, VECTOR4: 4, PLANE: 4, QUATERNION: 4,
          AABB: 6, BASIS: 9, TRANSFORM3D: 12, PROJECTION: 16}
_INTS = {VECTOR2I: 2, RECT2I: 4, VECTOR3I: 3, VECTOR4I: 4}
_PACKED_ELEMENTS = {PACKED_INT32: "<i", PACKED_INT64: "<q", PACKED_FLOAT32: "<f",
                    PACKED_FLOAT64: "<d"}
_PACKED_TUPLES = {PACKED_VECTOR2: 2, PACKED_VECTOR3: 3, PACKED_COLOR: 4, PACKED_VECTOR4: 4}


@dataclass(frozen=True)
class Math:
    """A Godot math value, by its Variant type and its components in declaration order."""
    kind: int
    values: tuple


class ObjectId(int):
    """An Object the game encoded as its id (EncodedObjectAsID), which is all a peer sends."""


class UnsupportedVariant(Exception):
    pass


def _string(buf: bytes, at: int) -> tuple[str, int]:
    (length,) = struct.unpack_from("<I", buf, at)
    at += 4
    text = buf[at:at + length].decode("utf-8", "replace")
    return text, at + length + (-length % 4)


def _container_type(buf: bytes, at: int, kind: int) -> int:
    if kind == 1:  # a builtin element type
        return at + 4
    if kind in (2, 3):  # a class name, or a script path
        return _string(buf, at)[1]
    return at


def decode(buf: bytes, at: int = 0) -> tuple[object, int]:
    (header,) = struct.unpack_from("<I", buf, at)
    at += 4
    kind = header & 0xFF
    wide = bool(header & FLAG_64)
    if kind == NIL:
        return None, at
    if kind == BOOL:
        return struct.unpack_from("<I", buf, at)[0] != 0, at + 4
    if kind == INT:
        return (struct.unpack_from("<q", buf, at)[0], at + 8) if wide else (struct.unpack_from("<i", buf, at)[0], at + 4)
    if kind == FLOAT:
        return (struct.unpack_from("<d", buf, at)[0], at + 8) if wide else (struct.unpack_from("<f", buf, at)[0], at + 4)
    if kind in (STRING, STRING_NAME):
        return _string(buf, at)
    if kind in _REALS:
        count, fmt, size = _REALS[kind], ("d" if wide else "f"), (8 if wide else 4)
        return Math(kind, struct.unpack_from(f"<{count}{fmt}", buf, at)), at + count * size
    if kind in _INTS:
        count = _INTS[kind]
        return Math(kind, struct.unpack_from(f"<{count}i", buf, at)), at + count * 4
    if kind == COLOR:
        return Math(kind, struct.unpack_from("<4f", buf, at)), at + 16
    if kind == RID:
        return Math(kind, struct.unpack_from("<Q", buf, at)), at + 8
    if kind == NODE_PATH:
        names, subnames, _flags = struct.unpack_from("<III", buf, at)
        at += 12
        parts = []
        for _ in range((names & 0x7FFFFFFF) + subnames):
            part, at = _string(buf, at)
            parts.append(part)
        return "/".join(parts), at
    if kind == OBJECT:
        if not header & FLAG_64:  # HEADER_DATA_FLAG_OBJECT_AS_ID shares the bit
            raise UnsupportedVariant("a full object, which a debugger peer never sends")
        return ObjectId(struct.unpack_from("<Q", buf, at)[0]), at + 8
    if kind == CALLABLE:
        return None, at
    if kind == SIGNAL:
        name, at = _string(buf, at)
        return (name, ObjectId(struct.unpack_from("<Q", buf, at)[0])), at + 8
    if kind == DICTIONARY:
        at = _container_type(buf, at, (header >> 16) & 3)
        at = _container_type(buf, at, (header >> 18) & 3)
        (count,) = struct.unpack_from("<I", buf, at)
        at += 4
        result = {}
        for _ in range(count & 0x7FFFFFFF):
            key, at = decode(buf, at)
            value, at = decode(buf, at)
            result[key if not isinstance(key, (list, dict)) else repr(key)] = value
        return result, at
    if kind == ARRAY:
        at = _container_type(buf, at, (header >> 16) & 3)
        (count,) = struct.unpack_from("<I", buf, at)
        at += 4
        items = []
        for _ in range(count & 0x7FFFFFFF):
            item, at = decode(buf, at)
            items.append(item)
        return items, at
    if kind == PACKED_BYTE:
        (count,) = struct.unpack_from("<I", buf, at)
        at += 4
        return bytes(buf[at:at + count]), at + count + (-count % 4)
    if kind in _PACKED_ELEMENTS:
        (count,) = struct.unpack_from("<I", buf, at)
        fmt = _PACKED_ELEMENTS[kind]
        size = struct.calcsize(fmt)
        return list(struct.unpack_from(f"<{count}{fmt[1]}", buf, at + 4)), at + 4 + count * size
    if kind == PACKED_STRING:
        (count,) = struct.unpack_from("<I", buf, at)
        at += 4
        strings = []
        for _ in range(count):
            text, at = _string(buf, at)
            strings.append(text.rstrip("\0"))
        return strings, at
    if kind in _PACKED_TUPLES:
        (count,) = struct.unpack_from("<I", buf, at)
        width = _PACKED_TUPLES[kind]
        size = 8 if wide and kind != PACKED_COLOR else 4
        fmt = "d" if size == 8 else "f"
        flat = struct.unpack_from(f"<{count * width}{fmt}", buf, at + 4)
        return [tuple(flat[i:i + width]) for i in range(0, len(flat), width)], at + 4 + count * width * size
    raise UnsupportedVariant(f"Variant type {kind}")


def encode(value: object) -> bytes:
    """What the editor sends: nil, bools, ints, strings and arrays of them."""
    if value is None:
        return struct.pack("<I", NIL)
    if isinstance(value, bool):
        return struct.pack("<II", BOOL, 1 if value else 0)
    if isinstance(value, int):
        return struct.pack("<Iq", INT | FLAG_64, value)
    if isinstance(value, str):
        raw = value.encode("utf-8")
        return struct.pack("<II", STRING, len(raw)) + raw + b"\0" * (-len(raw) % 4)
    if isinstance(value, (list, tuple)):
        return struct.pack("<II", ARRAY, len(value)) + b"".join(encode(item) for item in value)
    raise UnsupportedVariant(f"encoding a {type(value).__name__}")


class Wire:
    """The editor's end of one game's connection."""

    def __init__(self, connection: socket.socket) -> None:
        self.connection = connection
        self.pending = b""
        self.backlog: list[tuple[str, int, list]] = []

    def send(self, name: str, data: list, thread: int = MAIN_THREAD) -> None:
        body = encode([name, thread, data])
        self.connection.sendall(struct.pack("<I", len(body)) + body)

    def _receive(self, deadline: float) -> "tuple[str, int, list] | None":
        while True:
            if len(self.pending) >= 4:
                (length,) = struct.unpack_from("<I", self.pending)
                if len(self.pending) >= 4 + length:
                    body, self.pending = self.pending[4:4 + length], self.pending[4 + length:]
                    message, _ = decode(body)
                    name, thread, data = message
                    return name, thread, data
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.connection.settimeout(remaining)
            try:
                chunk = self.connection.recv(1 << 16)
            except socket.timeout:
                return None
            if not chunk:
                return None
            self.pending += chunk

    def expect(self, names: "set[str]", timeout: float) -> "tuple[str, int, list] | None":
        """The next message named one of `names`, set aside from anything else that arrives first,
        or None when `timeout` passes -- which is how "it did not stop" is asserted."""
        for index, message in enumerate(self.backlog):
            if message[0] in names:
                return self.backlog.pop(index)
        deadline = time.monotonic() + timeout
        while True:
            message = self._receive(deadline)
            if message is None:
                return None
            if message[0] in names:
                return message
            if message[0] not in ("output", "servers:profile_frame", "performance:profile_frame"):
                self.backlog.append(message)

@dataclass
class Frame:
    file: str
    line: int
    function: str

    @property
    def name(self) -> str:
        """The function without the parameter types a Verse frame is named with (`_Process(:float)`)."""
        return self.function.split("(", 1)[0]


class Stop:
    """What a stop shows: its reason, its stack and each frame's variables when asked."""

    def __init__(self, wire: Wire, enter: list, thread: int) -> None:
        self.wire = wire
        self.can_continue, self.reason, self.has_stack, self.thread = enter
        self.frames: list[Frame] = []
        wire.send("get_stack_dump", [], self.thread)
        dump = wire.expect({"stack_dump"}, 30)
        if dump is not None:
            data = dump[2]
            for i in range(1, 1 + int(data[0]), 3):
                self.frames.append(Frame(str(data[i]), int(data[i + 1]), str(data[i + 2])))

    @property
    def top(self) -> Frame:
        return self.frames[0] if self.frames else Frame("", 0, "")

    def variables(self, level: int = 0) -> "dict[str, tuple[int, object]]":
        """`Locals/<name>`, `Members/<name>` and `Globals/<name>`, the way the panel's inspector
        spells them, each as (the Variant type the game sent, the value)."""
        self.wire.send("get_stack_frame_vars", [level], self.thread)
        header = self.wire.expect({"stack_frame_vars"}, 30)
        found: dict[str, tuple[int, object]] = {}
        if header is None:
            return found
        for _ in range(int(header[2][0])):
            message = self.wire.expect({"stack_frame_var"}, 30)
            if message is None:
                break
            name, scope, var_type, value, _hint = message[2]
            found[f"{('Locals', 'Members', 'Globals')[scope]}/{name}"] = (var_type, value)
        return found

    def resume(self, command: str) -> None:
        self.wire.send(command, [], self.thread)


def line_of(path: Path, text: str) -> int:
    """The 1-based line of `path` that reads exactly `text`, ignoring trailing whitespace."""
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if line.rstrip() == text:
            return number
    raise SystemExit(f"{path} has no line reading {text!r}; the fixture and the cases disagree")


class Cases:
    def __init__(self) -> None:
        self.passed = self.failed = self.skipped = 0

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        if ok:
            self.passed += 1
            print(f"[{TAG}] {name}: ok", flush=True)
        else:
            self.failed += 1
            print(f"[{TAG}] {name}: FAIL" + (f" ({detail})" if detail else ""), flush=True)
        return ok

    def check_eq(self, name: str, got: object, expected: object) -> bool:
        return self.check(name, got == expected, f"got {got!r}, expected {expected!r}")

    def skip(self, name: str, why: str) -> None:
        self.skipped += 1
        print(f"[{TAG}] {name}: skip -- {why}", flush=True)

    def report(self) -> None:
        print(f"[{TAG}] {self.passed} passed, {self.failed} failed, {self.skipped} skipped", flush=True)


VERSE = "res://scripts/debug_play.verse"
CONTROL = "res://debugger/debug_control.gd"
SCENE = "res://debugger/debug_play.tscn"
# The two known defects, in the words tests/editor's editor_cases.gd records them under.
READY_DEFECT = ("known defect: the Verse debugger attaches from the language's _frame "
                "(VerseDebugger::sync_attachment), and a main scene's _Ready runs before the first "
                "one, so a breakpoint there never fires")
TWICE_DEFECT = ("known defect: a line holding a call reports its location again when the result "
                "lands, and should_break asks is_breakpoint of both, so a breakpoint there stops twice "
                "per arrival -- Continue stops on the same line once more")


def where(stop: "Stop | None") -> "tuple[str, int, str] | None":
    return (stop.top.file, stop.top.line, stop.top.name) if stop is not None else None


def stop_at(wire: Wire, timeout: float) -> "Stop | None":
    enter = wire.expect({"debug_enter"}, timeout)
    return Stop(wire, enter[2], int(enter[2][3])) if enter is not None else None


def drive(cases: Cases, wire: Wire, lines: dict[str, int]) -> None:
    visited: list[tuple[str, int]] = []

    def arrive(timeout: float = 60) -> "Stop | None":
        stop = stop_at(wire, timeout)
        if stop is not None:
            visited.append((stop.top.file, stop.top.line))
        return stop

    # GDScript control first: its node is the scene's first child, so its _ready runs first.
    stop = arrive(120)
    if not cases.check("GDScript control: a --breakpoints breakpoint stops the game", stop is not None
                       and stop.reason == "Breakpoint", "no debug_enter arrived" if stop is None else stop.reason):
        return
    cases.check_eq("GDScript control: the top frame is its line in _ready",
                   (stop.top.file, stop.top.line, stop.top.function), (CONTROL, lines["control"], "_ready"))
    variables = stop.variables()
    cases.check_eq("GDScript control: Stack Variables carries the local first", variables.get("Locals/first"), (INT, 10))
    stop.resume("next")
    stop = arrive(30)
    cases.check_eq("GDScript control: Step Over from a breakpoint on a line holding a call moves one line",
                   (stop.top.file, stop.top.line) if stop else None, (CONTROL, lines["control"] + 1))
    if stop is None:
        return
    stop.resume("continue")

    stop = arrive(60)
    if stop is not None and where(stop)[:2] == (VERSE, lines["ready"]):
        cases.check("a breakpoint in the main scene's _Ready stops", True)
        stop.resume("continue")
        stop = arrive(60)
    else:
        cases.skip("a breakpoint in the main scene's _Ready stops", READY_DEFECT)
    if not cases.check("a --breakpoints breakpoint stops a Verse script", stop is not None
                       and stop.reason == "Breakpoint", "no debug_enter arrived" if stop is None else stop.reason):
        return
    cases.check_eq("the stack's top frame is debug_play.verse at the breakpoint's line, in Walk",
                   where(stop), (VERSE, lines["start"], "Walk"))
    cases.check_eq("and the frame under it is the _Process that called it",
                   [(f.file, f.name) for f in stop.frames[1:2]], [(VERSE, "_Process")])

    stop.resume("next")
    stop = arrive(30)
    cases.check_eq("Step Over moves one line", where(stop), (VERSE, lines["spot"], "Walk"))
    if stop is None:
        return
    stop.resume("next")
    stop = arrive(30)
    cases.check("Step Over from a line that reports twice does not land on it again",
                stop is not None and where(stop)[1] != lines["spot"], f"it landed at {where(stop)}")
    cases.check_eq("and lands on the next line", where(stop), (VERSE, lines["call"], "Walk"))
    if stop is None:
        return
    variables = stop.variables()
    cases.check_eq("Stack Variables: the local Spot crosses as a Vector2",
                   variables.get("Locals/Spot"), (VECTOR2, Math(VECTOR2, (1.0, 2.0))))
    cases.check_eq("Stack Variables: the member Health crosses as an int", variables.get("Members/Health"), (INT, 7))
    cases.check_eq("Stack Variables: the member Where crosses as a Vector2",
                   variables.get("Members/Where"), (VECTOR2, Math(VECTOR2, (3.0, 4.0))))
    cases.check_eq("Stack Variables: the var member Frames crosses as an int", variables.get("Members/Frames"), (INT, 3))
    selves = [name for name in variables if name.split("/", 1)[1].lower() == "self"]
    cases.check("Stack Variables: self is under members and nowhere else",
                len(selves) == 1 and selves[0].startswith("Members/"), f"the rows naming self are {selves}")

    stop.resume("next")
    stop = arrive(30)
    cases.check("Step Over from a line holding a call does not land on it again when the result lands",
                stop is not None and where(stop)[1] != lines["call"], f"it landed at {where(stop)}")
    cases.check_eq("and lands on the next line, in the same function", where(stop), (VERSE, lines["call"] + 1, "Walk"))
    if stop is None:
        return
    stop.resume("continue")

    stop = arrive(30)
    if not cases.check("a second breakpoint stops at the call to Outer",
                       stop is not None and where(stop)[:2] == (VERSE, lines["outer_call"]),
                       f"it stopped at {where(stop)}"):
        return
    # Off before stepping, so a stop on this line again can only be the step's.
    wire.send("breakpoint", [VERSE, lines["outer_call"], False], stop.thread)
    depth = len(stop.frames)
    stop.resume("step")
    stop = arrive(30)
    cases.check("Step Into enters the Verse callee",
                stop is not None and stop.top.name == "Outer" and len(stop.frames) == depth + 1
                and stop.top.line in (lines["outer_decl"], lines["outer_first"]),
                f"it landed at {stop.frames if stop else None}")
    if stop is None:
        return
    cases.check("and the caller's frame is under it, at the call",
                len(stop.frames) > 1 and (stop.frames[1].file, stop.frames[1].line, stop.frames[1].name)
                == (VERSE, lines["outer_call"], "Walk"), f"the stack is {stop.frames}")
    stop.resume("out")
    stop = arrive(30)
    cases.check("Step Out returns to the caller",
                stop is not None and stop.top.name == "Walk" and len(stop.frames) == depth,
                f"it landed at {stop.frames if stop else None}")
    if stop is None:
        return
    stop.resume("continue")

    stop = arrive(30)
    if not cases.check("a breakpoint on another line holding a call stops",
                       stop is not None and where(stop)[:2] == (VERSE, lines["again"]), f"it stopped at {where(stop)}"):
        return
    stop.resume("continue")
    again = arrive(2)
    if again is None:
        cases.check("and Continue from it does not stop there again when the result lands", True)
    elif where(again)[:2] == (VERSE, lines["again"]) and again.reason == "Breakpoint":
        cases.skip("and Continue from it does not stop there again when the result lands", TWICE_DEFECT)
        again.resume("continue")
    else:
        cases.check("and Continue from it does not stop there again when the result lands", False,
                    f"it stopped at {where(again)}")
        again.resume("continue")

    # Helper has run three times by now -- from Walk, from Outer and from Walk again -- with its
    # last line armed throughout.
    cases.check("a breakpoint on a trailing bare expression never fires", (VERSE, lines["trailing"]) not in visited,
                f"it stopped there; the stops were {visited}")

    wire.send("breakpoint", [VERSE, lines["process"], True])
    stop = arrive(10)
    cases.check_eq("a breakpoint sent while the game runs arms, in _Process",
                   where(stop), (VERSE, lines["process"], "_Process"))
    if stop is None:
        return
    wire.send("breakpoint", [VERSE, lines["process"], False], stop.thread)
    stop.resume("continue")
    cases.check("and one removed while stopped stays removed", arrive(1.5) is None)

    wire.send("set_skip_breakpoints", [True])
    wire.send("breakpoint", [VERSE, lines["process"], True])
    cases.check("Skip Breakpoints: an armed line in _Process does not stop", arrive(1.5) is None)
    wire.send("set_skip_breakpoints", [False])
    stop = arrive(10)
    cases.check("and it stops there again once skipping is off",
                stop is not None and (stop.top.file, stop.top.line) == (VERSE, lines["process"]),
                f"it stopped at {(stop.top.file, stop.top.line) if stop else None}")
    if stop is None:
        return
    wire.send("breakpoint", [VERSE, lines["process"], False], stop.thread)
    stop.resume("continue")

    profile(cases, wire)


def _script_rows(data: list, signatures: dict[int, str]) -> "list[dict] | None":
    """A servers:profile_frame or profile_total's script functions, by signature: the last block of
    the frame, five values per function (servers_debugger.cpp, ServersProfilerFrame::serialize)."""
    at = 7
    for _ in range(int(data[6])):
        at += 2 + int(data[at + 1])
    count = int(data[at])
    rows = []
    for i in range(at + 1, at + 1 + count, 5):
        sig_id, calls, self_time, total_time, _internal = data[i:i + 5]
        rows.append({"signature": signatures.get(int(sig_id), f"SigErr {sig_id}"), "calls": int(calls),
                     "self": float(self_time), "total": float(total_time)})
    return rows


def _named(rows: list[dict], name: str) -> list[dict]:
    """The rows whose signature's last part is `name`, a Verse one's parameter types aside."""
    return [row for row in rows if row["signature"].split("::")[-1].split("(", 1)[0] == name]


def profile(cases: Cases, wire: Wire) -> None:
    signatures: dict[int, str] = {}

    def remember(messages: list) -> None:
        for _, _, data in messages:
            signatures[int(data[1])] = str(data[0])

    wire.send("profiler:servers", [True, [64, False]])
    frames = []
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        message = wire._receive(deadline)
        if message is None:
            break
        if message[0] == "servers:function_signature":
            remember([message])
        elif message[0] == "servers:profile_frame":
            frames.append(message[2])
    rows = [row for frame in frames for row in (_script_rows(frame, signatures) or [])]
    process = _named(rows, "debug_play._Process")
    cases.check("the profiler names a Verse function in a frame, with its call", any(r["calls"] >= 1 for r in process),
                f"the rows named {sorted({r['signature'] for r in rows})}")
    cases.check("and the profile{} block's own row", bool(_named(rows, "debug_play_tag")),
                f"the rows named {sorted({r['signature'] for r in rows})}")
    cases.check("GDScript control: the profiler names its _process",
                any(r["signature"].startswith(CONTROL) and r["signature"].endswith("::_process") for r in rows),
                f"the rows named {sorted({r['signature'] for r in rows})}")

    wire.send("profiler:servers", [False])
    total = None
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        message = wire._receive(deadline)
        if message is None:
            break
        if message[0] == "servers:function_signature":
            remember([message])
        elif message[0] == "servers:profile_total":
            total = message[2]
            break
    if not cases.check("servers:profile_total arrives when the profiler stops", total is not None):
        return
    accumulated = _script_rows(total, signatures) or []
    verse = [row for row in accumulated if ".verse::" in row["signature"]]
    described = [f"{r['signature'].rsplit('/', 1)[-1]} x{r['calls']}" for r in accumulated]
    cases.check("its accumulated table carries more than one Verse row", len(verse) > 1, f"it carries {described}")
    cases.check("every row names a function by GDScript's three-part signature",
                bool(accumulated) and all(len(r["signature"].split("::")) >= 3 and r["signature"].split("::")[-1]
                                          for r in accumulated), f"it carries {described}")
    # The stride trap: godot-cpp's struct is 32 bytes for elements that are 40, so a wrong stride
    # corrupts every Verse row after the bridge's first. Godot sorts the rows before sending them,
    # so every Verse row is held to it, and the two that count once per frame are held to each other.
    cases.check("every Verse row reads a sane call count and times, not only the first (the ProfilingInfo stride)",
                bool(verse) and all(0 < r["calls"] < 1_000_000 and 0 <= r["self"] <= r["total"] < 600 for r in verse),
                f"it carries {described}")
    process = _named(accumulated, "debug_play._Process")
    tagged = _named(accumulated, "debug_play_tag")
    cases.check("the accumulated _Process row counts every frame's call, not one frame's",
                bool(process) and process[0]["calls"] > 1, f"it carries {described}")
    cases.check("the accumulated table carries the profile{} block's row, called as often as _Process",
                bool(process) and bool(tagged) and tagged[0]["calls"] == process[0]["calls"], f"it carries {described}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--godot", required=True)
    parser.add_argument("--project", required=True, help="tests/integration, with the extension staged")
    args = parser.parse_args()
    project = Path(args.project)

    verse = project / "scripts" / "debug_play.verse"
    control = project / "debugger" / "debug_control.gd"
    lines = {
        "control": line_of(control, "\tvar second := work()"),
        "ready": line_of(verse, "\t\tPrint(\"debug_play ready\")"),
        "start": line_of(verse, "\t\tStart := 1"),
        "spot": line_of(verse, "\t\tSpot := vector2{X := 1.0, Y := 2.0}"),
        "call": line_of(verse, "\t\tTotal := Helper()"),
        "outer_call": line_of(verse, "\t\tDeeper := Outer()"),
        "again": line_of(verse, "\t\tAgain := Helper()"),
        "outer_decl": line_of(verse, "\tOuter<public>()<transacts>:int ="),
        "outer_first": line_of(verse, "\t\tStepped := Helper() + 1"),
        "trailing": line_of(verse, "\t\tInner"),
        "process": line_of(verse, "\t\tset Frames += 1"),
    }
    armed = [f"{CONTROL}:{lines['control']}"] + [f"{VERSE}:{lines[name]}" for name in
                                                  ("ready", "start", "outer_call", "again", "trailing")]

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]
    # The game blocks before its first frame when its stdout is a pipe nobody drains.
    log_path = Path(__file__).resolve().parent.parent / "bin" / "debug_wire_game.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    cases = Cases()
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        game = subprocess.Popen(
            [args.godot, "--headless", "--path", str(project), "--remote-debug", f"tcp://127.0.0.1:{port}",
             "--breakpoints", ",".join(armed), "--scene", SCENE],
            stdout=log, stderr=subprocess.STDOUT, env=godot_env.env_for(godot_env.DEFAULT_HOME))
        try:
            server.settimeout(120)
            try:
                connection, _ = server.accept()
            except socket.timeout:
                connection = None
            if cases.check("the game connects to the remote debugger", connection is not None):
                wire = Wire(connection)
                first = wire.expect({"set_pid"}, 30)
                cases.check("its first message is set_pid", first is not None)
                drive(cases, wire, lines)
                connection.close()
        finally:
            game.kill()
            game.wait(30)
            server.close()
    if cases.failed:
        tail = log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-25:]
        for line in tail:
            print(f"[{TAG}]   game: {line}")
    cases.report()
    return 1 if cases.failed else 0


if __name__ == "__main__":
    sys.exit(main())
