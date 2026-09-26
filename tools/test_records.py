"""Per-case records parsed from the test drivers' one-line-per-case output, and the comparison that
holds an exported run of tests/integration to the editor run's own case list.

Pure: no Godot, no UE and no subprocess, so tests/test_records/test_test_records.py exercises all of
it offline, against lines copied from a real run.

The drivers print one of these per case, and nothing here asks them to print anything else:

  [<tag>] <case>: ok
  [<tag>] <case>: FAIL
  [<tag>] <case>: FAIL (<detail>)          integration's `got X, expected Y` is split into its halves
  [<tag>] <case>: skip -- <reason>
  ok - <case>                              test_gen_verse_api.py, which prints no tag
  FAIL - <case> (<detail>)

and Chrome relays a page's console line as `"<text>", source: <url> (<line>)`, which is unwrapped
first. tests/integration's library also brackets a block an export skips under one name with
`begin editor-only section: <title>` and `end editor-only section`.
"""

from __future__ import annotations

import re
from dataclasses import asdict, dataclass, field

PASS = "pass"
FAIL = "fail"
SKIP = "skip"

# The tag-less format test_gen_verse_api.py prints.
PLAIN = ""

# tests/integration/test_cases.gd's EDITOR_ONLY and NO_THREADS_WHY, which is where a skip an
# exported run is allowed to make is declared.
EDITOR_ONLY = "editor only"
NO_THREADS_WHY = "a build without threads runs a pool task on the calling thread"

_CHROME_CONSOLE = re.compile(r'^"(.*)", source: \S+ \(\d+\)$')
_TAGGED = re.compile(r"^\[([^\]]+)\] (.*)$")
_PLAIN = re.compile(r"^(ok|FAIL) - (.*)$")
_SECTION_BEGIN = "begin editor-only section: "
_SECTION_END = "end editor-only section"
# Anchored at the end and tried at the earliest `: ` a name could stop at, so a reason or a detail
# may carry `: ok` inside it and still leave the case name whole.
_OUTCOME = re.compile(r": (?:(ok)|(FAIL)(?: \((.*)\)| -- (.*))?|skip -- (.*))$")
_GOT_EXPECTED = re.compile(r"^got (.*), expected (.*)$")


@dataclass
class Case:
    layer: str
    suite: str
    case: str
    status: str
    detail: str | None = None
    got: str | None = None
    expected: str | None = None
    # The editor-only section a case was printed inside, if any. Not part of the record's identity.
    section: str | None = field(default=None, compare=False)
    kind: str = "case"

    def to_json(self) -> dict:
        record = asdict(self)
        if record["section"] is None:
            del record["section"]
        return record


def unwrap_console(line: str) -> str:
    match = _CHROME_CONSOLE.match(line)
    return match.group(1) if match else line


def parse_outcome(rest: str) -> tuple[str, str, str | None] | None:
    """`<case>: ok|FAIL ...|skip -- ...` -> (case, status, detail), or None for any other line."""
    at = rest.find(": ")
    while at != -1:
        match = _OUTCOME.match(rest, at)
        if match is not None and at > 0:
            ok, _, fail_paren, fail_dash, skip_why = match.groups()
            name = rest[:at].strip()
            if ok:
                return name, PASS, None
            if skip_why is not None:
                return name, SKIP, skip_why
            return name, FAIL, fail_paren if fail_paren is not None else fail_dash
        at = rest.find(": ", at + 1)
    return None


def parse_cases(output: str, layer: str, suite: str, tag: str) -> list[Case]:
    """Every case line in `output` printed under `[tag]`, or in the plain format when tag is PLAIN."""
    cases: list[Case] = []
    section: str | None = None
    for raw in output.splitlines():
        line = unwrap_console(raw.strip())
        if tag == PLAIN:
            match = _PLAIN.match(line)
            if match is None:
                continue
            status = PASS if match.group(1) == "ok" else FAIL
            name, detail = match.group(2), None
            if status == FAIL and name.endswith(")") and " (" in name:
                name, detail = name[:name.rindex(" (")], name[name.rindex(" (") + 2:-1]
            cases.append(_case(layer, suite, name.strip(), status, detail, None))
            continue

        match = _TAGGED.match(line)
        if match is None or match.group(1) != tag:
            continue
        rest = match.group(2)
        if rest.startswith(_SECTION_BEGIN):
            section = rest[len(_SECTION_BEGIN):].strip()
            continue
        if rest.strip() == _SECTION_END:
            section = None
            continue
        parsed = parse_outcome(rest)
        if parsed is not None:
            cases.append(_case(layer, suite, *parsed, section))
    return cases


def _case(layer: str, suite: str, name: str, status: str, detail: str | None,
          section: str | None) -> Case:
    got = expected = None
    if status == FAIL and detail:
        split = _GOT_EXPECTED.match(detail)
        if split:
            got, expected = split.groups()
    return Case(layer, suite, name, status, detail, got, expected, section)


def duplicates(cases: list[Case]) -> list[str]:
    """Case names printed more than once, in first-seen order. A repeat makes a name-keyed
    comparison ambiguous, so it is the harness's failure rather than a case's."""
    seen: set[str] = set()
    repeated: list[str] = []
    for case in cases:
        if case.case in seen and case.case not in repeated:
            repeated.append(case.case)
        seen.add(case.case)
    return repeated


def summary_counts(output: str, tag: str) -> tuple[int, int, int] | None:
    """The last `[tag] N passed, M failed, K skipped` line's numbers."""
    found = None
    for raw in output.splitlines():
        line = unwrap_console(raw.strip())
        match = re.match(rf"^\[{re.escape(tag)}\] (\d+) passed, (\d+) failed, (\d+) skipped$", line)
        if match:
            found = tuple(int(group) for group in match.groups())
    return found


def is_editor_only(reason: str | None) -> bool:
    return reason is not None and (reason == EDITOR_ONLY or reason.startswith(EDITOR_ONLY + ": "))


@dataclass
class Comparison:
    """How an exported run's cases line up against the editor run's."""
    passed: int = 0
    skipped: int = 0
    by_section: int = 0
    sections: int = 0
    # Cases the exported run printed as failing, which are already failing records of their own.
    failed: list[str] = field(default_factory=list)
    # What is wrong with the case list itself: (case, why).
    problems: list[tuple[str, str]] = field(default_factory=list)


def compare_to_reference(reference: list[Case], exported: list[Case],
                         allowed_reasons: tuple[str, ...] = ()) -> Comparison:
    """Every case the editor run printed must appear in the exported run, passing or skipped for a
    reason the library marks editor-only (or one of `allowed_reasons`), and nothing may appear that
    the editor run did not print. A case inside an editor-only section is also accounted for by one
    editor-only skip named after that section.

    The editor run's own status is not consulted: a case it skipped (an autoload, which only an
    export has) may pass here, and a case it failed is the integration layer's to report.
    """
    result = Comparison()
    exported_by_name = {case.case: case for case in exported}
    sections = {case.section for case in reference if case.section}
    reference_names = {case.case for case in reference}

    def allowed_skip(case: Case) -> bool:
        return case.status == SKIP and (is_editor_only(case.detail) or case.detail in allowed_reasons)

    skipped_sections = {name for name in sections
                        if name in exported_by_name and exported_by_name[name].status == SKIP
                        and is_editor_only(exported_by_name[name].detail)}
    result.sections = len(skipped_sections)

    for ref in reference:
        got = exported_by_name.get(ref.case)
        if got is None:
            if ref.section in skipped_sections:
                result.by_section += 1
            else:
                result.problems.append((ref.case, "the editor run printed it and the exported run did not"))
        elif got.status == PASS:
            result.passed += 1
        elif allowed_skip(got):
            result.skipped += 1
        elif got.status == SKIP:
            result.problems.append((ref.case, f"skipped for a reason that is not editor-only: {got.detail}"))
        else:
            result.failed.append(ref.case)

    for got in exported:
        if got.case in reference_names:
            continue
        if got.case in skipped_sections:
            continue
        result.problems.append((got.case, "the exported run printed it and the editor run did not"))
    return result
