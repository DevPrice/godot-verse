#!/usr/bin/env python3
"""docs/architecture-review.md item 4 step 2: one test per measured claim.

CLAUDE.md marks a numeric claim about the mirror or the ABI with an invisible
`<!--fact:KEY-->` tag immediately before the number, e.g. `<!--fact:mirror.classes-->1036`, and
`tools/gen_verse_api.py`'s `write_facts` recomputes every one of those keys into docs/facts.json
on every run. This test reads both and fails the moment they disagree, so an engine drop that
moves a count is a red test naming the paragraph rather than a stale sentence nobody re-reads.

It also checks the citations a "measured" or "verified against the engine" sentence carries --
`(contract: probe/sleep_probe.verse)`, `(contract: godot/cyclic_resource_load_answers_null)` or
`(contract: tripwire/no_doc_comment_syntax)` -- against the three contract-layer namespaces those
citations can name: a `tests/verse_probe` fixture with a golden under `tests/verse_probe/expected/`,
one of `tools/run_godot_contract.py`'s case names, or one of `tools/run_tripwires.py`'s tripwires
(each of which must also be cited). A citation to a fixture or case that no longer exists fails here rather than staying a dead
link.

No pytest dependency: prints one line per case and exits non-zero if any case fails.
"""

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
CLAUDE_MD = REPO / "CLAUDE.md"
FACTS_PATH = REPO / "docs" / "facts.json"
PROBE_DIR = REPO / "tests" / "verse_probe"
PROBE_EXPECTED_DIR = PROBE_DIR / "expected"

FACT_TAG_RE = re.compile(r"<!--fact:([A-Za-z0-9_.]+)-->(-?\d+)")
CONTRACT_RE = re.compile(r"\(contract:\s*([^)]+)\)")

# A fact docs/facts.json carries that no CLAUDE.md tag cites, each with the reason it has no prose
# to attach to. Anything else uncited is either a fact this test should tag CLAUDE.md with, or a
# fact write_facts should stop computing -- so adding a row here without a reason is exactly how
# this allowlist would rot the way the prose it replaces did.
UNCITED_FACTS = {
    # docs/architecture-review.md item 4 step 5's own number, not a sentence in CLAUDE.md's prose.
    "abi.const_overrides_rows",
    # CLAUDE.md spells this "only those two accessors are failable" rather than in digits.
    "mirror.singletons_failable",
    # CLAUDE.md states the 38 `Tag...` constants (mirror.variant_tag_constants) but never the total
    # that includes TagNil.
    "mirror.variant_tags",
}

# tools/run_godot_contract.py's 9 case names (docs/architecture-review.md item 4 step 3), read out
# of that file's own docstring and `tests/godot_contract/test_main.gd`'s `_check`/`_check_eq`
# calls. Kept as a literal set rather than parsed out of either file: the names are string literals
# in two different call shapes across a Python file and a GDScript file, and a citation naming one
# that got renamed or removed is exactly the drift this test exists to catch -- a parser that
# tracked the rename would never fail.
GODOT_CONTRACT_CASES = {
    "profiling_info_registration_stride",
    "profiling_info_struct_field_count",
    "lookup_result_script_path_key",
    "booleanize_empty_variant_is_false",
    "cyclic_resource_load_answers_null",
    "singleton_class_ip",
    "singleton_class_navigation_server_2d",
    "singleton_class_display_server_is_gdsoftclass",
    "b30_cyclic_load_log_sentence",
}

# tools/run_tripwires.py's tripwire ids (docs/architecture-review.md item 4 step 4), literal for
# GODOT_CONTRACT_CASES' reason: a rename there is the drift this test catches. A `tripwire/<id>`
# citation must name one of these, and every one of them must be cited somewhere in CLAUDE.md, so a
# tripwire cannot exist without the paragraph it retires.
TRIPWIRES = {
    "attribute_takes_one_argument",
    "no_doc_comment_syntax",
    "subscribable_event_unreleased",
    "no_verse_lsp_binary",
}
RUN_TRIPWIRES = REPO / "tools" / "run_tripwires.py"

failures = []


def check(name: str, ok: bool, detail: str = "") -> None:
    print(f"{'ok' if ok else 'FAIL'} - {name}" + ("" if ok else f" ({detail})"))
    if not ok:
        failures.append(name)


def unique(seen: set, case_name: str) -> str:
    """A case name printed twice in one suite is a harness failure (CLAUDE.md, `run_tests.py`'s
    own rule) -- so a fact or a citation repeated on one line gets `#2`, `#3`, ... appended."""
    name, n = case_name, 1
    while name in seen:
        n += 1
        name = f"{case_name}#{n}"
    seen.add(name)
    return name


def check_facts(text: str, facts: dict) -> None:
    tags = list(FACT_TAG_RE.finditer(text))
    check("claude_md_has_fact_tags", len(tags) > 0, "no <!--fact:...--> tags found in CLAUDE.md")

    seen = set()
    cited = set()
    for m in tags:
        key, value = m.group(1), int(m.group(2))
        line = text.count("\n", 0, m.start()) + 1
        # Named by key and occurrence, not by line, so an unrelated edit to CLAUDE.md does not
        # rename every case below it; the line is in the detail, where a failure needs it.
        case_name = unique(seen, f"tag {key}")
        cited.add(key)
        if key not in facts:
            check(case_name, False, f"CLAUDE.md:{line}: docs/facts.json has no fact named {key!r}")
            continue
        check(case_name, facts[key] == value,
              f"CLAUDE.md:{line} says {value}, docs/facts.json says {facts[key]}")

    for key in sorted(facts):
        if key in UNCITED_FACTS:
            continue
        check(f"fact {key} is cited in CLAUDE.md", key in cited,
              f"docs/facts.json has {key!r}, no <!--fact:{key}--> tag cites it, and it is not in "
              f"test_claims.py's UNCITED_FACTS")

    for key in sorted(UNCITED_FACTS):
        check(f"uncited fact {key} still exists", key in facts,
              f"UNCITED_FACTS names {key!r}, which docs/facts.json no longer has -- update the "
              f"allowlist or tag CLAUDE.md with it")


def check_contract_citations(text: str) -> None:
    citations = list(CONTRACT_RE.finditer(text))
    check("claude_md_has_contract_citations", len(citations) > 0,
          "no (contract: ...) citations found in CLAUDE.md")

    seen = set()
    cited_tripwires = set()
    for m in citations:
        line = text.count("\n", 0, m.start()) + 1
        for raw in m.group(1).split(","):
            cite = raw.strip()
            case_name = unique(seen, f"contract {cite}")
            where = f"CLAUDE.md:{line}"
            if cite.startswith("probe/") and cite.endswith(".verse"):
                fixture = cite[len("probe/"):]
                src = PROBE_DIR / fixture
                golden = PROBE_EXPECTED_DIR / f"{fixture}.txt"
                check(case_name, src.is_file() and golden.is_file(),
                      f"{where}: {src} is a file: {src.is_file()}; {golden} is a file: {golden.is_file()}")
            elif cite.startswith("godot/"):
                name = cite[len("godot/"):]
                check(case_name, name in GODOT_CONTRACT_CASES,
                      f"{where}: {name!r} is not one of tools/run_godot_contract.py's case names")
            elif cite.startswith("tripwire/"):
                name = cite[len("tripwire/"):]
                cited_tripwires.add(name)
                check(case_name, name in TRIPWIRES,
                      f"{where}: {name!r} is not one of tools/run_tripwires.py's tripwire ids")
            else:
                check(case_name, False,
                      f"{where}: {cite!r} names none of probe/<fixture>.verse, godot/<case>, tripwire/<id>")

    runner = RUN_TRIPWIRES.read_text(encoding="utf-8")
    for name in sorted(TRIPWIRES):
        check(f"tripwire {name} is cited in CLAUDE.md", name in cited_tripwires,
              f"no (contract: tripwire/{name}) citation in CLAUDE.md")
        check(f"tripwire {name} is in run_tripwires.py", f'Tripwire("{name}",' in runner,
              f"tools/run_tripwires.py declares no Tripwire({name!r}, ...)")


def main() -> int:
    text = CLAUDE_MD.read_text(encoding="utf-8")
    facts = json.loads(FACTS_PATH.read_text(encoding="utf-8"))

    check_facts(text, facts)
    check_contract_citations(text)

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED: {failures}")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
