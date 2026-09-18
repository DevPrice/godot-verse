"""The hover harness: every tooltip a Verse script editor could draw, and what is wrong with it.

    python tools/probe_hover.py                      # dodge-the-creeps
    python tools/probe_hover.py --project tests/integration
    python tools/probe_hover.py --dump rows.jsonl    # the corpus, for reading by hand
    python tools/probe_hover.py --show Speed         # every row for one symbol

An instrument rather than a test, in the sense tools/README.md means: it reports findings and
exits 0 unless `--strict` is passed. What it reproduces is the editor's hover, which has no other
test -- `ScriptTextEditor::_show_symbol_tooltip` calls `lookup_code` on the language and renders
the result in the editor's own C++, and `_lookup_code` is a virtual, which ClassDB stores as
metadata rather than as a callable MethodBind. `VerseScriptLanguage::probe_hover` is the seam the
driver calls instead; it splices the cursor marker the way CodeEdit does and answers one row per
word and run of columns.

The rules below are what GDScript does, read out of Godot 4.7-stable:

  modules/gdscript/gdscript_editor.cpp   GDScriptLanguage::lookup_code, and the local walk at
                                         SuiteNode::Local -- CONSTANT is the only kind that
                                         answers LOCAL_CONSTANT; PARAMETER, FOR_VARIABLE and
                                         PATTERN_BIND all answer LOCAL_VARIABLE
  editor/script/script_text_editor.cpp   _show_symbol_tooltip: which result types become a
                                         tooltip at all, and what they are asked for
  editor/doc/editor_help.cpp             the label: "Local Constant" / "Local Variable", and
                                         that a CLASS_* result is looked up in the class docs
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from run_tests import find_engine, find_godot, stage_extension  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
DRIVER = "hover_probe_driver.gd"

# ScriptLanguageExtension::LookupResultType, core/object/script_language_extension.h.
LOOKUP_TYPES = {
    0: "SCRIPT_LOCATION",
    1: "CLASS",
    2: "CLASS_CONSTANT",
    3: "CLASS_PROPERTY",
    4: "CLASS_METHOD",
    5: "CLASS_SIGNAL",
    6: "CLASS_ENUM",
    7: "CLASS_TBD_GLOBALSCOPE",
    8: "CLASS_ANNOTATION",
    9: "LOCAL_CONSTANT",
    10: "LOCAL_VARIABLE",
}

# vh_lookup_kind, include/verse_host_abi.h.
HOST_KINDS = {
    -1: "none",
    0: "unknown",
    1: "data",
    2: "function",
    3: "class",
    4: "module",
    5: "type_alias",
    6: "enum",
}

OK = 0


def collect(project: Path, godot: Path, engine: Path) -> list[dict]:
    """Runs the driver in the project and returns the rows it printed."""
    # The library the project holds, not the one just built, is what a Godot run loads -- and a
    # stale one reports the old answers with nothing to say it did. Two measurements of a fix that
    # had already landed read as the fix doing nothing before this line existed. run_tests stages
    # the same way before each of its Godot layers.
    stale = stage_extension(project)
    if stale:
        raise SystemExit(f"probe_hover: {stale}")

    driver = project / DRIVER
    shutil.copy2(REPO / "tools" / "hover_probe.gd", driver)
    env = dict(os.environ, UE_ROOT=str(engine))
    try:
        completed = subprocess.run(
            [str(godot), "--headless", "--path", str(project),
             "--script", f"res://{DRIVER}", "--quit-after", "600"],
            capture_output=True, text=True, env=env, check=False,
        )
    finally:
        driver.unlink(missing_ok=True)
        # Godot writes a .uid beside a script it has seen; the project is committed.
        (project / (DRIVER + ".uid")).unlink(missing_ok=True)

    rows = []
    done = False
    for line in completed.stdout.splitlines():
        if line.startswith("HOVER\t"):
            rows.append(json.loads(line[len("HOVER\t"):]))
        elif line.startswith("HOVER_DONE\t"):
            done = True
    if not done:
        sys.stderr.write(completed.stdout[-4000:])
        sys.stderr.write(completed.stderr[-4000:])
        raise SystemExit("probe_hover: the driver did not finish -- see the output above")
    return rows


def where(row: dict) -> str:
    span = str(row["column"]) if row["column"] == row["column_end"] else f"{row['column']}-{row['column_end']}"
    return f"{row['path']}:{row['line'] + 1}:{span}"


def described(row: dict) -> str:
    kind = LOOKUP_TYPES.get(row["type"], str(row["type"]))
    if row["result"] != OK:
        return "no tooltip"
    if kind in ("LOCAL_CONSTANT", "LOCAL_VARIABLE"):
        label = "Local Constant" if kind == "LOCAL_CONSTANT" else "Local Variable"
        doc = row["doc_type"] or "<no type>"
        return f'"{label}" {row["symbol"]}: {doc}'
    if kind == "CLASS":
        return f"class docs for {row['class_name']}"
    if kind.startswith("CLASS_"):
        return f"{kind[len('CLASS_'):].lower()} docs for {row['class_name']}.{row['class_member']}"
    return kind


def host_of(row: dict) -> str:
    kind = HOST_KINDS.get(row.get("host_kind", -1), str(row.get("host_kind")))
    bits = [kind]
    if row.get("host_owner"):
        bits.append(f"of {row['host_owner']}")
    if row.get("host_is_parameter"):
        bits.append("parameter")
    if row.get("host_is_var"):
        bits.append("var")
    if row.get("host_is_definition"):
        bits.append("at its declaration")
    return " ".join(bits)


class Finding:
    def __init__(self, rule: str, row: dict, what: str, expected: str) -> None:
        self.rule = rule
        self.row = row
        self.what = what
        self.expected = expected

    def line(self) -> str:
        return (f"[{self.rule}] {where(self.row)} `{self.row['symbol']}` "
                f"({host_of(self.row)})\n"
                f"         is: {self.what}\n"
                f"   expected: {self.expected}")


def code_rows(rows: list[dict]) -> list[dict]:
    """Rows a hover could reach as code. A comment or a string literal is neither, and is its own
    rule (H6) rather than a row the label rules have anything to say about."""
    return [r for r in rows if r["token"] not in ("comment", "string", "escape")]


def visible(row: dict) -> tuple:
    """What the tooltip would draw. Two rows that agree on this are one hover to an author, which
    is what H7 has to compare -- the host fields beside them are diagnosis, not what is seen."""
    if row["result"] != OK:
        return ("none",)
    return (row["type"], row["class_name"], row["class_member"],
            row["doc_type"], row["description"], row["location"])


def rules(rows: list[dict]) -> list[Finding]:
    findings: list[Finding] = []
    # A class the project declares, which is the one class name the editor has a *script* doc for.
    declared = {Path(row["path"]).stem for row in rows}

    for row in code_rows(rows):
        kind = LOOKUP_TYPES.get(row["type"])
        answered = row["result"] == OK
        local = kind in ("LOCAL_CONSTANT", "LOCAL_VARIABLE")
        host_kind = HOST_KINDS.get(row.get("host_kind", -1), "none")
        host_resolved = host_kind not in ("none", "unknown")

        # H1. "Local Constant" is what Godot draws for LOCAL_CONSTANT, and GDScript reaches it for
        # one thing only: a `const` declared inside a function body (gdscript_editor.cpp's walk of
        # SuiteNode::Local). A class, a module, an enum, a type or a function wearing that label is
        # the label being used as a fallback for "nothing better to say".
        #
        # It reports ~250 rows over tests/integration and ~12 over dodge-the-creeps, and none of
        # them is a *type* any more: every one is a function, and they fall into four families that
        # have nothing better to say, which is the line drawn in by-hand-findings.md B23.
        #
        #   a global the bridge invented   MakeVariant, AsInt, VariantInt -- no Godot counterpart
        #   a wrapper class's method       godot_array.GetInt, signal(t).Await -- likewise
        #   Verse's own                    event, Sqrt, BitOr -- Verse's, not Godot's
        #   the project's own              a second class in a file and its members, which no
        #                                  script doc is registered for (see _lookup_code)
        #
        # All four keep the local result because it is the only one that carries prose, and the
        # prose is the comment above the declaration. A *new* finding here is one that leaves those
        # four -- most of all anything a mirrored class or a Godot function stands behind.
        if answered and local and host_kind in ("class", "module", "enum", "type_alias", "function"):
            findings.append(Finding(
                "H1", row, described(row),
                f"a result that names a {host_kind} -- this is not a local"))

        # H2. A parameter is LOCAL_VARIABLE in GDScript: PARAMETER, FOR_VARIABLE and PATTERN_BIND
        # share the arm with VARIABLE, and only CONSTANT takes the other one.
        if answered and kind == "LOCAL_CONSTANT" and row.get("host_is_parameter"):
            findings.append(Finding(
                "H2", row, described(row),
                '"Local Variable", which is what GDScript calls a parameter'))

        # H3. A member of a class the project declares is a property or a method of it, and saying
        # so is what fetches the comment above the declaration out of the script's own doc.
        if (answered and local and not row.get("host_is_parameter")
                and row.get("host_owner") in declared):
            findings.append(Finding(
                "H3", row, described(row),
                f"a property or method of {row['host_owner']}"))

        # H4. An empty tooltip: a label, the symbol, and nothing else in the box. GDScript fills
        # doc_type for every local it answers (GDScriptDocGen::doctype_from_datatype).
        if answered and local and not row["doc_type"] and not row["description"]:
            findings.append(Finding(
                "H4", row, "a tooltip with neither a type nor a description in it",
                "a type, the way GDScript fills doc_type for every local"))

        # H5. The host resolved the identifier, knows what it is, and the editor draws nothing.
        # Silence is a legitimate answer where there is nothing to say -- `void` has no Godot page
        # and GDScript answers nothing for it either -- so the test is that something was in hand.
        #
        # A type is what "in hand" meant, and a *class* has none: LookupSymbol fills Type for a
        # data definition and a function only. So a generated binding, whose whole failure was a
        # resolved class with nothing on this side to say about it, sat in the corpus reporting
        # nothing at all -- which is why the kind is asked here beside the type.
        #
        # It reports two rows over tests/integration, both `cancelable`, and they are one standing
        # family rather than a defect of the day: a class of Verse's own library has no Godot page
        # to name and documents itself with a `@doc` attribute where it documents itself at all.
        # A *new* row here is one that leaves that family.
        if not answered and host_resolved and (row.get("host_type") or host_kind == "class"):
            described_as = f" of type {row['host_type']}" if row.get("host_type") else ""
            findings.append(Finding(
                "H5", row, "no tooltip",
                f"a tooltip: the host resolved this to a {host_kind}{described_as}"))

        # H9. A jump names the script twice or it is only half a jump. Godot renamed the field
        # between 4.7 and 4.8 -- a `Ref<Script>` there, a `script_path` here -- and a result
        # filling one of them is, in the other editor, a location with no script beside it,
        # which ScriptTextEditor reads as a line in the file being *edited*. So a ctrl+click
        # scrolls the open file to its own top rather than opening the file the answer named.
        # A same-file jump fills neither and is not this: it *means* the file being edited.
        if row.get("location", -1) >= 0 and bool(row.get("script_path")) != bool(row.get("has_script")):
            findings.append(Finding(
                "H9", row, "a jump that names the script one way and not the other",
                "both `script` and `script_path`, so the click lands in 4.7 and 4.8 alike"))

    # H6. A word in prose. `# the script looks up a node` is not code, and a hover over it that
    # answers Godot's Script or Node documentation is the mirror's lowercase class names colliding
    # with English. GDScript has the same shortcut ahead of its parse and almost never meets it,
    # because its class names are PascalCase and nobody writes `Node` in a sentence.
    for row in rows:
        if row["token"] in ("comment", "string") and row["result"] == OK:
            findings.append(Finding(
                "H6", row, described(row),
                "no tooltip: the pointer is over a comment or a string, not over code"))

    # H8. One name, answered in one place and silent in another. Neither half is wrong on its own
    # -- which is why this is a rule about the corpus rather than about a row -- but a name the
    # editor can describe where it is used and cannot describe where it is declared is a hole in
    # the walk rather than a decision. This is what found `player := class(area2d)` resolving from
    # every file except its own.
    answers: dict[str, set] = {}
    for row in code_rows(rows):
        answers.setdefault(row["symbol"], set()).add(row["result"] == OK)
    for symbol, outcomes in sorted(answers.items()):
        if outcomes != {True, False}:
            continue
        silent = [r for r in code_rows(rows) if r["symbol"] == symbol and r["result"] != OK]
        spoke = next(r for r in code_rows(rows) if r["symbol"] == symbol and r["result"] == OK)
        findings.append(Finding(
            "H8", silent[0],
            f"no tooltip here, and {described(spoke)} at {where(spoke)}"
            + (f" (and {len(silent) - 1} more silent)" if len(silent) > 1 else ""),
            "the same answer wherever the name is written"))

    # H7. One word, two answers, decided by where in it the pointer happened to land. An author
    # hovers a word; which of its columns the mouse is over is not something they choose.
    seen: dict[tuple, list[dict]] = {}
    for row in code_rows(rows):
        seen.setdefault((row["path"], row["line"], row["word"]), []).append(row)
    for _, group in sorted(seen.items()):
        run: list[dict] = []
        for row in sorted(group, key=lambda r: r["column"]):
            if run and visible(run[-1]) == visible(row):
                run[-1] = dict(row, column=run[-1]["column"])
                continue
            run.append(row)
        if len(run) > 1:
            findings.append(Finding(
                "H7", run[0],
                " / ".join(f"cols {r['column']}-{r['column_end']}: {described(r)}" for r in run),
                "one answer for the whole word, whichever column the pointer is over"))

    return findings


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--project", default="dodge-the-creeps",
                        help="the Godot project to probe (default: dodge-the-creeps)")
    parser.add_argument("--godot", help="the Godot binary (default: GODOT, then PATH)")
    parser.add_argument("--engine", help="the Unreal checkout (default: UE_ROOT)")
    parser.add_argument("--dump", help="write every row to this file as JSON lines")
    parser.add_argument("--rows", help="read the rows from a --dump file instead of running Godot")
    parser.add_argument("--show", help="print every row for one symbol and stop")
    parser.add_argument("--strict", action="store_true", help="exit non-zero when anything is found")
    args = parser.parse_args()

    if args.rows:
        rows = [json.loads(line) for line in Path(args.rows).read_text(encoding="utf-8").splitlines() if line]
    else:
        project = Path(args.project)
        if not project.is_absolute():
            project = REPO / project
        if not (project / "project.godot").is_file():
            raise SystemExit(f"probe_hover: {project} is not a Godot project")
        godot = find_godot(args.godot)
        if godot is None:
            raise SystemExit("probe_hover: no Godot binary -- set GODOT or pass --godot")
        engine = find_engine(args.engine)
        if engine is None:
            raise SystemExit("probe_hover: no Unreal checkout -- set UE_ROOT or pass --engine")
        rows = collect(project, godot, engine)

    if args.dump:
        Path(args.dump).write_text(
            "".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")

    if args.show:
        for row in rows:
            if row["symbol"] == args.show:
                print(f"{where(row)}  {row['token']:<20} {described(row):<50} [{host_of(row)}]")
        return

    # A project that did not build answers nothing for everything, and every rule below is about
    # what an answer *said* -- so a failed build reads as a clean corpus. It cost one false
    # all-clear to learn that: the fixture had a member colliding with a mirrored property, the
    # whole project refused to compile, and 17433 hover positions reported no findings.
    if not any(row.get("host_kind", -1) > 0 for row in rows):
        raise SystemExit("probe_hover: the host resolved nothing anywhere -- the project did not "
                         "build. Run it in Godot and read the compile errors first.")

    findings = rules(rows)
    by_rule: dict[str, int] = {}
    for finding in findings:
        by_rule[finding.rule] = by_rule.get(finding.rule, 0) + 1
        print(finding.line())
        print()

    words = len({(r["path"], r["line"], r["column"]) for r in rows})
    print(f"[probe_hover] {len(rows)} rows over {words} hover positions in "
          f"{len({r['path'] for r in rows})} file(s)")
    if not findings:
        print("[probe_hover] nothing found")
    else:
        print("[probe_hover] " + ", ".join(f"{rule}: {count}" for rule, count in sorted(by_rule.items())))
    if args.strict and findings:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
