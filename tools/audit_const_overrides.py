#!/usr/bin/env python3
"""Finds Godot methods that answer a value, are not marked `const`, and provably do not mutate.

`gen_verse_api.py` decides a mirrored method's effect from Godot's own `is_const`: const and
answering becomes `<reads>`, everything else `<transacts>` (docs/phase-4.5-design.md §3). That flag
is applied unevenly -- `Tween::is_running` is `bool is_running() { return running; }` and is not
marked const -- so `CONST_OVERRIDES` in the generator carries the exceptions, and this is what
produced it. Run it by hand when the list is revisited; it is not part of `run_tests.py`, because it
needs a **Godot source checkout** that nothing else here requires:

    python tools/audit_const_overrides.py --godot ../godot

It prints rows ready to paste into `CONST_OVERRIDES`, and reports on stderr what it rejected.

**The bar is deliberately strict, and the direction is deliberately one-sided.** A wrong `<reads>`
claims more than it should and would let a mutation escape a rollback the label promised; a
conservative `<transacts>` merely claims less than it could, which costs an author nothing but a
`<transacts>` on their own helper. So a method is accepted only when all three hold:

  - its body is exactly `return <member>;` -- one statement, no call, no assignment, optionally
    negated or one field deep. Anything else proved nothing about what runs;
  - the returned expression is not a literal. `return 0;` is a base-class stub a subclass overrides,
    not a read;
  - the method name is declared `virtual` nowhere in the tree. This over-rejects on purpose: a
    same-named virtual on an unrelated class is enough to disqualify, because matching a
    declaration to its class would need a C++ parser and the cost of being wrong is asymmetric.
    `Tween::is_valid` is rejected this way and is genuinely non-virtual.
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
EXTENSION_API = REPO / "godot-cpp" / "gdextension" / "extension_api.json"

# `bool Tween::is_running() {\n\treturn running;\n}` -- a definition taking no arguments whose whole
# body is one return of a name. Tab-indented, which is Godot's own style throughout.
TRIVIAL_BODY = re.compile(
    r'^[\w:<>,\s\*&]+?\b(?P<cls>\w+)::(?P<fn>\w+)\(\s*\)\s*(?:const\s*)?\{\s*\n'
    r'\treturn (?P<expr>!?[\w]+(?:\.\w+)?);\s*\n'
    r'\}', re.M)

VIRTUAL_DECL = re.compile(r'\bvirtual\b[^;{]*?\b(\w+)\s*\(')
LITERAL = re.compile(r'^(?:!?\d+|true|false|nullptr|NULL)$')

SKIP_DIRS = (".git", "thirdparty", "bin", "misc")


def read_tree(godot: Path):
    """Every .cpp and .h in the checkout, minus the directories that hold no Godot classes."""
    cpp, headers = [], []
    for root, dirs, files in os.walk(godot):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in files:
            if not name.endswith((".cpp", ".h")):
                continue
            try:
                text = (Path(root) / name).read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            (cpp if name.endswith(".cpp") else headers).append(text)
    return cpp, headers


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", default=str(REPO.parent / "godot"),
                        help="A Godot *source* checkout (default: ../godot)")
    parser.add_argument("--api", default=str(EXTENSION_API))
    args = parser.parse_args()

    godot = Path(args.godot)
    if not (godot / "core").is_dir():
        print(f"[audit] {godot} is not a Godot source checkout -- pass --godot", file=sys.stderr)
        return 2

    api = json.loads(Path(args.api).read_text(encoding="utf-8"))
    candidates = [(c["name"], m["name"])
                  for c in api["classes"]
                  for m in (c.get("methods") or [])
                  if not m.get("is_virtual") and not m.get("is_static")
                  and not m.get("is_const") and m.get("return_value") is not None]

    cpp, headers = read_tree(godot)
    print(f"[audit] {len(cpp)} .cpp and {len(headers)} .h in {godot}", file=sys.stderr)

    bodies = {}
    for text in cpp:
        for m in TRIVIAL_BODY.finditer(text):
            bodies.setdefault((m.group("cls"), m.group("fn")), m.group("expr"))

    virtual_names = set()
    for text in headers:
        virtual_names.update(m.group(1) for m in VIRTUAL_DECL.finditer(text))

    kept, rejected = [], []
    for godot_class, method in candidates:
        expr = bodies.get((godot_class, method))
        if expr is None:
            continue
        if LITERAL.match(expr):
            rejected.append((godot_class, method, "returns a literal, so it is a stub"))
        elif method in virtual_names:
            rejected.append((godot_class, method, "the name is declared virtual somewhere"))
        else:
            kept.append((godot_class, method, expr))

    print(f"[audit] {len(kept)} accepted, {len(rejected)} rejected, "
          f"out of {len(candidates)} non-const answering methods", file=sys.stderr)
    for godot_class, method, why in sorted(rejected):
        print(f"[audit]   rejected {godot_class}.{method}: {why}", file=sys.stderr)

    for godot_class, method, expr in sorted(kept):
        print(f'    ("{godot_class}", "{method}"),  # return {expr};')
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
