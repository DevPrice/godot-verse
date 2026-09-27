#!/usr/bin/env python3
"""The contract layer's tripwires: each asserts that a limitation of Epic's the bridge works around
*still holds*, so the day Epic lifts one is a failing case naming the adapter to retire and the
section that explains it, rather than a workaround nobody notices has become unnecessary.

docs/architecture-review.md item 4 step 4. docs/tripwires.md is the table an author reads; TRIPWIRES
below is the same table, and tests/claims/test_claims.py holds CLAUDE.md's `(contract:
tripwire/<id>)` citations to its ids.

    python tools/run_tripwires.py --engine <UE checkout>

Each check is one of three kinds, and a check whose input is absent is skipped and says so:

  source  reads the UE checkout's own files -- needs only the checkout
  probe   compiles a tests/verse_probe fixture through bin/verse_probe.exe and the editor host, and
          looks for the refusal in what it printed
  unit    runs `verse_host_unit.exe <repo> --tripwires`, which asks the host's own adapter a
          question no transcript shows (tests/host_unit/unit_tripwires.verse)

One line per check, `[tripwire] <id>/<check>: ok`, and a FAIL line carries the adapter and the
section after the detail.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

REPO = Path(__file__).resolve().parent.parent
PROBE_EXE = REPO / "bin" / "verse_probe.exe"
PROBE_DIR = REPO / "tests" / "verse_probe"
TAG = "tripwire"


@dataclass(frozen=True)
class Tripwire:
    id: str
    limitation: str
    adapter: str
    design: str


TRIPWIRES = {
    t.id: t
    for t in (
        Tripwire("attribute_takes_one_argument",
                 "an attribute takes one argument: GetAttributeTextValue refuses a tuple (SOL-972) "
                 "and an overloaded attribute constructor cannot be referenced",
                 "AttributeArgument (host/Private/HostEngineAdapters.h), and every reader that "
                 "splits one string: ReadRpcConfig, @export_flags",
                 "docs/tripwires.md; docs/phase-4b-design.md \"Stage 6's `@rpc`, and an attribute "
                 "that may not be overloaded\"; CLAUDE.md \"An attribute may take only one argument\""),
        Tripwire("no_doc_comment_syntax",
                 "Verse has no doc-comment syntax, and @doc is reachable only through "
                 "using { /Verse.org/Native }",
                 "DocOf (host/Private/HostEngineAdapters.h) and src/verse_doc_markup's "
                 "verse_doc_comment_above",
                 "docs/tripwires.md; docs/by-hand-findings.md B38, B41; CLAUDE.md \"Verse has no "
                 "doc-comment syntax\""),
        Tripwire("subscribable_event_unreleased",
                 "the Verse book's subscribable_event is not in this drop, and "
                 "subscribable_event_intrnl is <epic_internal>",
                 "SignalVerseEvent (host/Private/HostSignals.cpp) and the signal(t)/event(t) member "
                 "types",
                 "docs/tripwires.md; docs/signal-declaration.md 12; CLAUDE.md \"The Verse book's subscribable_event "
                 "does not exist in this drop\""),
        Tripwire("no_verse_lsp_binary",
                 "no executable links uLangLSP, which is a message-type library",
                 "tools/run_verse_lsp.py's find_lsp_exe, and the host's own lookup and completion "
                 "(host/Private/HostLookup.cpp)",
                 "docs/tripwires.md; docs/editor-tooling.md \"uLangLSP\""),
    )
}

OK, FAIL, SKIP = "ok", "FAIL", "skip"


@dataclass
class Context:
    engine: Path | None

    @property
    def host_dll(self) -> Path | None:
        return self.engine / "Engine" / "Binaries" / "Win64" / "verse_host.dll" if self.engine else None

    @property
    def host_unit(self) -> Path | None:
        return self.engine / "Engine" / "Binaries" / "Win64" / "verse_host_unit.exe" if self.engine else None


Outcome = tuple[str, str]  # (status, detail or skip reason)


def run_probe(ctx: Context, fixture: str) -> tuple[str | None, str]:
    """The probe's whole output for one compile-only fixture, or None and why it could not run."""
    if ctx.engine is None:
        return None, "no Unreal checkout -- set UE_ROOT or pass --engine"
    if not ctx.host_dll.is_file():
        return None, f"{ctx.host_dll} not built -- run tools/build_host.py"
    if not PROBE_EXE.is_file():
        return None, f"{PROBE_EXE} not built -- run tools/build_verse_probe.py"
    argv = [str(PROBE_EXE), str(ctx.host_dll), str(ctx.engine / "Engine"), str(PROBE_DIR / fixture)]
    result = subprocess.run(argv, cwd=str(PROBE_EXE.parent), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace", timeout=180)
    return result.stdout, ""


def probe_says(fixture: str, pattern: str) -> Callable[[Context], Outcome]:
    def check(ctx: Context) -> Outcome:
        output, why = run_probe(ctx, fixture)
        if output is None:
            return SKIP, why
        if re.search(pattern, output):
            return OK, ""
        return FAIL, f"tests/verse_probe/{fixture} no longer says /{pattern}/"
    return check


def engine_file(ctx: Context, relative: str) -> tuple[Path | None, str]:
    if ctx.engine is None:
        return None, "no Unreal checkout -- set UE_ROOT or pass --engine"
    path = ctx.engine / relative
    if not path.is_file():
        return None, f"{path} is missing -- the engine layout moved, so this check needs its path updated"
    return path, ""


def source_matches(relative: str, pattern: str, meaning: str) -> Callable[[Context], Outcome]:
    def check(ctx: Context) -> Outcome:
        path, why = engine_file(ctx, relative)
        if path is None:
            # A moved file is a change worth reading, not a missing prerequisite.
            return (SKIP, why) if ctx.engine is None else (FAIL, why)
        text = path.read_text(encoding="utf-8", errors="replace")
        if re.search(pattern, text, re.MULTILINE):
            return OK, ""
        return FAIL, f"{relative} no longer {meaning}"
    return check


def comment_kinds(ctx: Context) -> Outcome:
    path, why = engine_file(ctx, "Engine/Source/Runtime/VerseCompiler/Public/uLang/Syntax/VstNode.h")
    if path is None:
        return (SKIP, why) if ctx.engine is None else (FAIL, why)
    text = path.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"struct Comment\b.*?enum class EType\s*:\s*\w+\s*\{([^}]*)\}", text, re.DOTALL)
    if not match:
        return FAIL, "VstNode.h's Vst::Comment has no EType enum any more"
    kinds = [kind.strip() for kind in match.group(1).split(",") if kind.strip()]
    if kinds == ["block", "line", "ind", "frag"]:
        return OK, ""
    return FAIL, f"Vst::Comment::EType is now {{{', '.join(kinds)}}}, not {{block, line, ind, frag}}"


def load_run_verse_lsp():
    spec = importlib.util.spec_from_file_location("run_verse_lsp", REPO / "tools" / "run_verse_lsp.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def no_lsp_executable(ctx: Context) -> Outcome:
    if ctx.engine is None:
        return SKIP, "no Unreal checkout -- set UE_ROOT or pass --engine"
    found = load_run_verse_lsp().find_lsp_exe(ctx.engine)
    if found is not None:
        return FAIL, f"run_verse_lsp.py's find_lsp_exe found {found}"
    binaries = ctx.engine / "Engine" / "Binaries" / "Win64"
    named = sorted(p.name for p in binaries.glob("*.exe") if re.search(r"lsp|languageserver", p.name, re.I))
    if named:
        return FAIL, f"{binaries} holds {', '.join(named)}, which run_verse_lsp.py does not look for"
    return OK, ""


def no_program_links_ulanglsp(ctx: Context) -> Outcome:
    if ctx.engine is None:
        return SKIP, "no Unreal checkout -- set UE_ROOT or pass --engine"
    roots = [ctx.engine / "Engine" / "Source", ctx.engine / "Engine" / "Plugins"]
    programs = []
    for root in roots:
        for build_cs in root.rglob("*.Build.cs"):
            if "Programs" not in build_cs.parts:
                continue
            if "uLangLSP" in build_cs.read_text(encoding="utf-8", errors="replace"):
                programs.append(str(build_cs.relative_to(ctx.engine)))
    if programs:
        return FAIL, f"a Program module names uLangLSP: {', '.join(programs)}"
    return OK, ""


CHECKS: list[tuple[str, str, Callable[[Context], Outcome]]] = [
    ("attribute_takes_one_argument", "overloaded_constructor_unreferenceable",
     probe_says("rpc_attribute_probe.verse",
                r"error 3502: Referencing an overloaded function without immediately calling it is not yet implemented")),
    ("attribute_takes_one_argument", "getattributetextvalue_skips_a_tuple",
     source_matches("Engine/Source/Runtime/VerseCompiler/Private/uLang/Semantics/Attributable.cpp",
                    r"GetNodeType\(\)\s*!=\s*EAstNodeType::Invoke_MakeTuple",
                    "declines an attribute whose argument is a MakeTuple")),
    ("no_doc_comment_syntax", "comment_kinds", comment_kinds),
    ("no_doc_comment_syntax", "doc_attribute_needs_using",
     probe_says("doc_attribute_reject.verse", r"error 3506: Unknown identifier `doc`")),
    ("subscribable_event_unreleased", "book_spelling_unknown",
     probe_says("subscribable_event_reject.verse", r"error 3506: Unknown identifier `subscribable_event`")),
    ("subscribable_event_unreleased", "intrnl_still_epic_internal",
     source_matches("Engine/Plugins/Verse/Verse/Source/Verse/Verse/Verse/Event.native.verse",
                    r"^subscribable_event_intrnl(?:<\w+>)*<epic_internal>(?:<\w+>)*\(t:type\)",
                    "declares subscribable_event_intrnl(t) <epic_internal>")),
    ("no_verse_lsp_binary", "no_executable", no_lsp_executable),
    ("no_verse_lsp_binary", "no_program_links_ulanglsp", no_program_links_ulanglsp),
]

# The checks verse_host_unit --tripwires prints itself, so a run where it printed none of them is a
# failure by name rather than a shorter log.
UNIT_CHECKS = [
    ("attribute_takes_one_argument", "one_string_reads"),
    ("attribute_takes_one_argument", "tuple_unreadable"),
]


def retire_note(tripwire_id: str) -> str:
    tripwire = TRIPWIRES[tripwire_id]
    return (f"the limitation is gone ({tripwire.limitation}): retire {tripwire.adapter}; "
            f"see {tripwire.design}")


def say(tripwire_id: str, check: str, status: str, detail: str) -> None:
    name = f"{tripwire_id}/{check}"
    if status == OK:
        print(f"[{TAG}] {name}: ok")
    elif status == SKIP:
        print(f"[{TAG}] {name}: skip -- {detail}")
    else:
        print(f"[{TAG}] {name}: FAIL ({detail} -- {retire_note(tripwire_id)})")
    sys.stdout.flush()


def run_unit(ctx: Context) -> dict[str, tuple[str, str]]:
    """verse_host_unit's own tripwire lines, keyed `<id>/<check>`, as (status, detail)."""
    reason = None
    if ctx.engine is None:
        reason = "no Unreal checkout -- set UE_ROOT or pass --engine"
    elif not ctx.host_unit.is_file():
        reason = f"{ctx.host_unit} not built -- run tools/build_host.py --target VerseHostUnit"
    if reason is not None:
        return {f"{i}/{c}": (SKIP, reason) for i, c in UNIT_CHECKS}

    result = subprocess.run([str(ctx.host_unit), str(REPO), "--tripwires"], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace", timeout=300)
    seen: dict[str, tuple[str, str]] = {}
    for line in result.stdout.splitlines():
        match = re.match(rf"^\[{TAG}\] (\S+): (ok|FAIL(?: \((.*)\))?)$", line.strip())
        if match:
            seen[match.group(1)] = (OK, "") if match.group(2) == "ok" else (FAIL, match.group(3) or "")
    for i, c in UNIT_CHECKS:
        seen.setdefault(f"{i}/{c}", (FAIL, f"verse_host_unit --tripwires exited {result.returncode} "
                                           f"without printing it"))
    return seen


def find_engine(explicit: str | None) -> Path | None:
    for candidate in (explicit, os.environ.get("UE_ROOT")):
        if candidate:
            path = Path(candidate)
            return path if path.is_dir() else None
    guess = REPO.parent / "UnrealEngine"
    return guess if guess.is_dir() else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--engine", help="the Unreal checkout (default: UE_ROOT, then ../UnrealEngine)")
    args = parser.parse_args()
    ctx = Context(find_engine(args.engine))

    counts = {OK: 0, FAIL: 0, SKIP: 0}
    for tripwire_id, check, run in CHECKS:
        status, detail = run(ctx)
        counts[status] += 1
        say(tripwire_id, check, status, detail)
    for name, (status, detail) in run_unit(ctx).items():
        tripwire_id, _, check = name.partition("/")
        if tripwire_id not in TRIPWIRES:
            status, detail = FAIL, f"verse_host_unit printed a tripwire {tripwire_id!r} that TRIPWIRES does not name"
            tripwire_id = next(iter(TRIPWIRES))
        counts[status] += 1
        say(tripwire_id, check, status, detail)

    print(f"[{TAG}] {counts[OK]} passed, {counts[FAIL]} failed, {counts[SKIP]} skipped")
    return 1 if counts[FAIL] else 0


if __name__ == "__main__":
    sys.exit(main())
