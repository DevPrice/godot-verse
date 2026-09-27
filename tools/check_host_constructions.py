#!/usr/bin/env python3
"""Refuses a host-side NewObject that does not go through NewHostObject.

Every vh_object runs a block clause that asks the host for a Godot peer (R-NODE-3), and the host
constructs one far more often than a script does: a script instance, a mirror wrapper, the reading
device the export defaults come off, the bare vh_object a dead handle crosses as. Each of those
already has its peer, or deliberately has none, and says so by opening a scope around the
construction. One that does not mints a Godot object nothing frees, while a working scene looks
entirely normal -- so NewHostObject is the only host-side construction of one, and this check is what
makes that a rule a new call site cannot miss.

A C++ compiler cannot see the rule: NewObject is the engine's template, called with a UClass chosen
at run time. VerseVM's own construction is not a NewObject at all (VClass::NewUObject allocates with
StaticAllocateObject), so a run-time test in the peer path cannot tell a host construction that
skipped the scope from a script's `helper{}` either. What is left is the source, and the source is
small enough to read: every NewObject under host/Private must be one of the ALLOWED sites below.

Run by the units layer (so CI runs it) and by build_host.py before it stages anything, so a bypass
fails the host build rather than the next leak hunt. Prints one line per case, the shape
tools/test_records.py parses as PLAIN.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HOST_PRIVATE = REPO / "host" / "Private"

# File -> (how many NewObject calls it may make, why). A construction of anything that is not a
# vh_object belongs here with its reason; a construction of a vh_object belongs in NewHostObject.
ALLOWED: dict[str, tuple[int, str]] = {
    "HostPeers.cpp": (2, "NewHostObject itself: one NewObject under each of its two scopes"),
    "HostMarshal.cpp": (1, "NewReferenceWrapper builds a godot_ref, which is not a vh_object and has no peer"),
}

CALL_RE = re.compile(r"\b(?:NewObject|StaticConstructObject_Internal)\s*[<(]")
DEFINITION_RE = re.compile(r"\bNewHostObject\s*\(\s*UClass\s*\*\s*\w+\s*,\s*FHostPeer\s+\w+\s*\)\s*\{")


def strip_comments_and_strings(text: str) -> str:
    """The code a compiler would see, with every comment and literal blanked out."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def host_sources(root: Path) -> list[Path]:
    return sorted(p for p in root.iterdir()
                  if p.suffix in (".cpp", ".h") and not p.name.endswith(".gen.h"))


def violations(root: Path = HOST_PRIVATE) -> list[tuple[str, bool, str]]:
    """(case name, passed, detail) for every file under root, and for the rule's own anchors."""
    cases = []
    counts: dict[str, int] = {}
    defined_in = []
    for path in host_sources(root):
        code = strip_comments_and_strings(path.read_text(encoding="utf-8", errors="replace"))
        counts[path.name] = len(CALL_RE.findall(code))
        if DEFINITION_RE.search(code):
            defined_in.append(path.name)

    for name, count in sorted(counts.items()):
        allowed, why = ALLOWED.get(name, (0, ""))
        detail = (f"{name} makes {count} NewObject call(s) where {allowed} are allowed"
                  + (f" ({why})" if why else "")
                  + " -- construct a vh_object with NewHostObject, which opens the peer scope; "
                    "anything else goes in tools/check_host_constructions.py's ALLOWED with its reason")
        cases.append((f"constructions in {name}", count == allowed, detail))

    for name in sorted(ALLOWED):
        cases.append((f"allowed file {name} exists", name in counts,
                      f"ALLOWED names {name}, which host/Private no longer has"))

    cases.append(("NewHostObject is defined once", len(defined_in) == 1,
                  f"host sources defining NewHostObject(UClass*, FHostPeer): {defined_in}"))
    return cases


def main() -> int:
    failed = 0
    for name, ok, detail in violations():
        print(f"{'ok' if ok else 'FAIL'} - {name}" + ("" if ok else f" ({detail})"))
        failed += 0 if ok else 1
    print()
    if failed:
        print(f"{failed} check(s) FAILED")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
