"""The completion harness: every popup a Verse script editor could raise, and what is wrong in it.

    python tools/probe_complete.py                          # dodge-the-creeps
    python tools/probe_complete.py --project tests/integration
    python tools/probe_complete.py --at member --limit 0    # every `.` in the project
    python tools/probe_complete.py --dump rows.jsonl        # the corpus, for reading by hand
    python tools/probe_complete.py --show Position          # every row offering one name

An instrument rather than a test, in the sense tools/README.md means: it reports findings and
exits 0 unless `--strict` is passed. What it reproduces is the completion popup, which has no
other test -- `CodeTextEditor::_complete_request` calls `complete_code` on the language and
CodeEdit renders the result, and `_complete_code` is a virtual, which ClassDB stores as metadata
rather than as a callable MethodBind. `VerseScriptLanguage::probe_complete` is the seam the driver
calls instead.

**It measures the answer, not the popup.** Whether CodeEdit raises one at all is decided by a
table of trigger characters the editor hard-codes (editor/gui/code_editor.cpp:2100) and cancels
against in `_filter_code_completion_candidates` (scene/gui/code_edit.cpp) -- neither of which any
answer from here reaches. That half is by-hand, and the steps are in docs/by-hand-findings.md.

A position costs an analysis, which a hover does not: `_complete_code` substitutes a placeholder
for the identifier being typed, so the text the host is asked about differs per caret. Hence
`--limit`, and hence positions chosen here rather than walked in the seam.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from run_tests import find_engine, find_godot, stage_extension  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
DRIVER = "complete_probe_driver.gd"
POSITIONS = "complete_probe_positions.json"

IDENT = re.compile(r"[A-Za-z0-9_]")
WORD_BEFORE = re.compile(r"[A-Za-z0-9_]+$")


def reserved_words() -> set[str]:
    """Verse's own, read out of the generated header rather than listed here.

    A `{` is only an archetype when what stands in front of it is a name: `array{}`, `map{}`,
    `option{X}` and `spawn{}` borrow the same braces and take no field names. That is the test
    `archetype_class_end` makes in verse_script_language.cpp, and making a different one here
    would report the bridge for declining a position it is right to decline.
    """
    header = (REPO / "src" / "verse_keywords.h").read_text(encoding="utf-8")
    body = header.split("reserved_words[] = {", 1)[1].split("};", 1)[0]
    return set(re.findall(r'"([^"]+)"', body))


# ScriptLanguageExtension::CodeCompletionKind, core/object/script_language_extension.h.
KINDS = {
    0: "class", 1: "function", 2: "signal", 3: "variable", 4: "member",
    5: "enum", 6: "constant", 7: "node_path", 8: "file_path", 9: "plain_text",
}


def positions_in(source: str, wanted: set[str], reserved: set[str]) -> list[tuple[int, int, str]]:
    """Carets worth asking about, as (line, byte column, what kind of position it is).

    The column is a byte offset into the line, which is how probe_hover reports one and how the
    seam reads one back.
    """
    found: list[tuple[int, int, str]] = []
    for line_number, line in enumerate(source.split("\n")):
        # A comment is not a position: _complete_code declines one outright, so every caret past a
        # `#` would be a row saying nothing. Strings are left in, because completion inside one is
        # a real answer (a node path, a signal name) and worth probing.
        code = line.split("#", 1)[0] if not line.lstrip().startswith("#") else ""
        raw = code.encode("utf-8")
        for at, byte in enumerate(raw):
            char = chr(byte)
            before = chr(raw[at - 1]) if at > 0 else ""
            if char == "." and "member" in wanted:
                # Right after the dot, where the prefix is empty: the snapshot answers first and
                # the analysis refines it, and both halves are worth a row.
                found.append((line_number, at + 1, "member"))
            elif char == "{" and "field" in wanted:
                word = WORD_BEFORE.search(raw[:at].decode("utf-8", "ignore"))
                if word and word.group() not in reserved:
                    found.append((line_number, at + 1, "field"))
            elif char == "?" and "named" in wanted and before in "([,":
                found.append((line_number, at + 1, "named"))
            elif char == "(" and "call" in wanted:
                found.append((line_number, at + 1, "call"))
            elif "scope" in wanted and IDENT.match(char) and not IDENT.match(before or " ") \
                    and before not in ".":
                # One character into a bare identifier, which is where a statement-position popup
                # actually opens: _complete_code declines an empty prefix there.
                found.append((line_number, at + 1, "scope"))
    return found


def collect(project: Path, godot: Path, engine: Path, wanted: set[str], limit: int) -> list[dict]:
    """Runs the driver in the project and returns the rows it printed."""
    # The library the project holds, not the one just built, is what a Godot run loads -- and a
    # stale one reports the old answers with nothing to say it did. probe_hover.py has the story.
    stale = stage_extension(project)
    if stale:
        raise SystemExit(f"probe_complete: {stale}")

    reserved = reserved_words()
    by_path: dict[str, list[list[int]]] = {}
    kinds: dict[tuple[str, int, int], str] = {}
    total = 0
    for path in sorted(project.rglob("*.verse")):
        if "addons" in path.parts:
            continue
        res = "res://" + path.relative_to(project).as_posix()
        source = path.read_text(encoding="utf-8").replace("\r\n", "\n")
        chosen = positions_in(source, wanted, reserved)
        if limit:
            chosen = chosen[: max(0, limit - total)]
        if not chosen:
            continue
        by_path[res] = [[line, column] for line, column, _ in chosen]
        for line, column, kind in chosen:
            kinds[(res, line, column)] = kind
        total += len(chosen)
        if limit and total >= limit:
            break

    if not by_path:
        raise SystemExit("probe_complete: no positions to ask about")

    driver = project / DRIVER
    positions = project / POSITIONS
    shutil.copy2(REPO / "tools" / "complete_probe.gd", driver)
    positions.write_text(json.dumps(by_path), encoding="utf-8")
    env = dict(os.environ, UE_ROOT=str(engine))
    try:
        completed = subprocess.run(
            [str(godot), "--headless", "--path", str(project),
             "--script", f"res://{DRIVER}", "--quit-after", "10000"],
            capture_output=True, text=True, env=env, check=False,
        )
    finally:
        driver.unlink(missing_ok=True)
        positions.unlink(missing_ok=True)
        # Godot writes a .uid beside a script it has seen; the project is committed.
        (project / (DRIVER + ".uid")).unlink(missing_ok=True)

    rows = []
    done = False
    for line in completed.stdout.splitlines():
        if line.startswith("COMPLETE\t"):
            row = json.loads(line[len("COMPLETE\t"):])
            row["at"] = kinds.get((row["path"], row["line"], row["column"]), "?")
            rows.append(row)
        elif line.startswith("COMPLETE_DONE\t"):
            done = True
    if not done:
        sys.stderr.write(completed.stdout[-4000:])
        sys.stderr.write(completed.stderr[-4000:])
        raise SystemExit("probe_complete: the driver did not finish -- see the output above")
    return rows


def where(row: dict) -> str:
    return f"{row['path']}:{row['line'] + 1}:{row['column']}"


# The generated accessors' spelling, and the reason this is a name rule rather than a flag: a
# completion option carries a display name and a kind and nothing else -- the host decides what an
# item *is*, and by the time one reaches here that decision has already been made. A user method
# genuinely called `ValueGetter` would be reported; it is an instrument, and the line says which.
ACCESSOR = re.compile(r"^[A-Za-z0-9_]+(Getter|Setter)\(")


class Finding:
    def __init__(self, rule: str, row: dict, what: str, expected: str) -> None:
        self.rule = rule
        self.row = row
        self.what = what
        self.expected = expected

    def line(self) -> str:
        return (f"[{self.rule}] {where(self.row)} at a {self.row['at']} position "
                f"(behind `{self.row['trigger']}`)\n"
                f"         is: {self.what}\n"
                f"   expected: {self.expected}")


def rules(rows: list[dict]) -> list[Finding]:
    findings: list[Finding] = []

    for row in rows:
        names = [option["display"] for option in row["options"]]

        # C1. An option no author could write. A class var's `<getter>`/`<setter>` takes an
        # `accessor` parameter nothing can construct, and only the compiler ever names one -- at
        # the point it rewrites a read or a write of the var the attributes are on. There are
        # about 4,380 of them across the mirror, two per Godot property, and offered they crowd
        # out the member the author is reaching for.
        accessors = [name for name in names if ACCESSOR.match(name)]
        if accessors:
            findings.append(Finding(
                "C1", row,
                f"{len(accessors)} of {len(names)} options are class var accessors: "
                + ", ".join(sorted(accessors)[:6]),
                "no accessor: DescribeCompletion refuses one the way it refuses a generated "
                "constructor"))

        # C2. A position the bridge narrows to a short, certain list and then answers nothing for.
        # Every other name is *refused* by the compiler at one of these, so an empty answer is not
        # a conservative one -- it is the popup having nothing to draw.
        if row["at"] in ("field", "named") and row["count"] == 0:
            findings.append(Finding(
                "C2", row,
                "no options",
                "the archetype's fields, or the callee's named parameters"))

        # C3. The popup opened on the snapshot and the analysis then replaced its whole contents.
        # Not a defect -- it is R-TOOL-3's design, the answer arriving at once and refining in
        # place -- but a position where the first answer shares nothing with the second is one
        # where an author picked from a list that was wrong, so the count is worth watching.
        if row["refined"] and row["first_count"] and row["count"]:
            first = {option["display"] for option in row["first_options"]}
            if not (first & set(names)):
                findings.append(Finding(
                    "C3", row,
                    f"{row['first_count']} options at once, then {row['count']} with none in common",
                    "the first answer to be a subset of the refined one"))

    return findings


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--project", default="dodge-the-creeps",
                        help="the Godot project to probe (default: dodge-the-creeps)")
    parser.add_argument("--godot", help="the Godot binary (default: GODOT, then PATH)")
    parser.add_argument("--engine", help="the Unreal checkout (default: UE_ROOT)")
    parser.add_argument("--at", default="member,field,named",
                        help="which positions to ask about: member, field, named, call, scope")
    parser.add_argument("--limit", type=int, default=120,
                        help="stop after this many positions; 0 for all (each costs an analysis)")
    parser.add_argument("--dump", help="write every row to this file as JSON lines")
    parser.add_argument("--rows", help="read the rows from a --dump file instead of running Godot")
    parser.add_argument("--show", help="print every row that offers one name and stop")
    parser.add_argument("--strict", action="store_true", help="exit non-zero when anything is found")
    args = parser.parse_args()

    if args.rows:
        rows = [json.loads(line) for line in Path(args.rows).read_text(encoding="utf-8").splitlines() if line]
    else:
        project = Path(args.project)
        if not project.is_absolute():
            project = REPO / project
        if not (project / "project.godot").is_file():
            raise SystemExit(f"probe_complete: {project} is not a Godot project")
        godot = find_godot(args.godot)
        if godot is None:
            raise SystemExit("probe_complete: no Godot binary -- set GODOT or pass --godot")
        engine = find_engine(args.engine)
        if engine is None:
            raise SystemExit("probe_complete: no Unreal checkout -- set UE_ROOT or pass --engine")
        rows = collect(project, godot, engine, set(args.at.split(",")), args.limit)

    if args.dump:
        Path(args.dump).write_text(
            "".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")

    if args.show:
        for row in rows:
            hit = [o["display"] for o in row["options"] if o["display"].startswith(args.show)]
            if hit:
                print(f"{where(row)}  {row['at']:<8} {', '.join(hit)}")
        return

    # A project that did not build answers nothing for everything, and every rule above is about
    # what an answer *said* -- so a failed build would read as a clean corpus. probe_hover.py cost
    # one false all-clear to learn that.
    if not any(row["count"] for row in rows):
        raise SystemExit("probe_complete: nothing answered anywhere -- the project did not build, "
                         "or the host is absent. Run it in Godot and read the errors first.")

    findings = rules(rows)
    by_rule: dict[str, int] = {}
    for finding in findings:
        by_rule[finding.rule] = by_rule.get(finding.rule, 0) + 1
        print(finding.line())
        print()

    offered = sum(row["count"] for row in rows)
    print(f"[probe_complete] {len(rows)} positions, {offered} options, in "
          f"{len({r['path'] for r in rows})} file(s)")
    if not findings:
        print("[probe_complete] nothing found")
    else:
        print("[probe_complete] " + ", ".join(f"{rule}: {count}" for rule, count in sorted(by_rule.items())))
    if args.strict and findings:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
