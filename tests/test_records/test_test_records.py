"""tools/test_records.py: the case-line parser and the comparison an exported run is held to.

One line per case, exit status 1 on any failure. The driver lines below are copied from real runs.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import test_records as tr  # noqa: E402

FAILURES = 0


def check(name: str, ok: bool, detail: str = "") -> None:
    global FAILURES
    if ok:
        print(f"[test_records] {name}: ok")
    else:
        FAILURES += 1
        print(f"[test_records] {name}: FAIL" + (f" ({detail})" if detail else ""))


def parse(text: str, tag: str = "integration") -> list:
    return tr.parse_cases(text, "integration", "integration", tag)


def one(text: str, tag: str = "integration"):
    cases = parse(text, tag)
    return cases[0] if len(cases) == 1 else None


def test_parse() -> None:
    case = one("[integration] a module reaches a root definition with nothing imported: ok")
    check("an ok line is a pass named by everything before the colon",
          case is not None and case.status == tr.PASS
          and case.case == "a module reaches a root definition with nothing imported")

    case = one("[integration] the edited project builds: FAIL (got int 1, expected int 0)")
    check("a FAIL line keeps its detail and splits got from expected",
          case is not None and case.status == tr.FAIL and case.got == "int 1"
          and case.expected == "int 0")

    case = one("[integration] and reports itself a tool script: skip -- editor only: the source is a stub in an export")
    check("a skip's reason may carry a colon without cutting the name short",
          case is not None and case.case == "and reports itself a tool script"
          and case.detail == "editor only: the source is a stub in an export")

    case = one('"[integration] a signal payload: ok", source: http://127.0.0.1:62025/index.js (452)')
    check("Chrome's console wrapper is taken off", case is not None and case.case == "a signal payload")

    case = one("[smoke] resolve vh_init: FAIL (symbol not found)", tag="smoke")
    check("another tag's FAIL with a parenthesised reason", case is not None and case.detail == "symbol not found")

    case = one("ok - a predicate is <decides>", tag=tr.PLAIN)
    check("the tag-less generator format", case is not None and case.status == tr.PASS
          and case.case == "a predicate is <decides>")
    case = one("FAIL - the class count (got 3, want 4)", tag=tr.PLAIN)
    check("its FAIL detail", case is not None and case.case == "the class count" and case.detail == "got 3, want 4")

    noise = "\n".join([
        "[integration] 572 passed, 0 failed, 5 skipped",
        "[verse_lexer_test] ALL PASS",
        "[smoke] exit code: 0",
        "[integration] begin editor-only section: hover",
        "[integration] end editor-only section",
        "[smoke] a smoke step: ok",
    ])
    check("summaries, banners, section markers and other tags are not cases", parse(noise) == [])

    sectioned = parse("\n".join([
        "[integration] before: ok",
        "[integration] begin editor-only section: the script editor's hover tooltip",
        "[integration] inside: ok",
        "[integration] end editor-only section",
        "[integration] after: ok",
    ]))
    check("a case inside a section carries it and none outside does",
          [c.section for c in sectioned] == [None, "the script editor's hover tooltip", None])

    check("the summary line's counts are read",
          tr.summary_counts('"[integration] 517 passed, 0 failed, 13 skipped", source: x (1)', "integration")
          == (517, 0, 13))


def test_duplicates() -> None:
    cases = parse("\n".join([
        "[integration] and a `<private>` one: ok",
        "[integration] something else: ok",
        "[integration] and a `<private>` one: ok",
    ]))
    check("a repeated case name is found", tr.duplicates(cases) == ["and a `<private>` one"])
    check("and a list without one has none", tr.duplicates(cases[:2]) == [])


EDITOR_RUN = "\n".join([
    "[integration] load marshal.verse: ok",
    "[integration] a second build publishes a new generation: ok",
    "[integration] the worker task ran: ok",
    "[integration] the Verse autoload is in the tree: skip -- --script replaces the main loop, so no autoload is set up",
    "[integration] begin editor-only section: the script editor's hover tooltip",
    "[integration] hover_probe.verse answers hovers: ok",
    "[integration] a parameter is a Local Constant, not a Local Variable: ok",
    "[integration] end editor-only section",
    "[integration] transactions.verse compiles: ok",
])

EXPORTED_RUN = [
    "[integration] load marshal.verse: ok",
    "[integration] a second build publishes a new generation: skip -- editor only",
    "[integration] the worker task ran: ok",
    "[integration] the Verse autoload is in the tree: ok",
    "[integration] the script editor's hover tooltip: skip -- editor only: no analysis in an exported game",
    "[integration] transactions.verse compiles: ok",
]


def compare(lines: list[str], allowed: tuple[str, ...] = ()) -> tr.Comparison:
    return tr.compare_to_reference(parse(EDITOR_RUN), parse("\n".join(lines)), allowed)


def test_comparison() -> None:
    green = compare(EXPORTED_RUN)
    check("an export matching the editor run is green", not green.problems and not green.failed,
          str(green.problems))
    check("and every editor case is accounted for",
          (green.passed, green.skipped, green.by_section, green.sections) == (4, 1, 2, 1),
          str((green.passed, green.skipped, green.by_section, green.sections)))

    missing = compare([line for line in EXPORTED_RUN if "transactions.verse" not in line])
    check("a case the export stopped printing is named",
          [name for name, _ in missing.problems] == ["transactions.verse compiles"], str(missing.problems))

    extra = compare(EXPORTED_RUN + ["[integration] only in the export: ok"])
    check("a case the editor never printed is named",
          [name for name, _ in extra.problems] == ["only in the export"], str(extra.problems))

    wrong_reason = compare([line.replace("skip -- editor only", "skip -- the node was not there")
                            if "second build" in line else line for line in EXPORTED_RUN])
    check("a skip whose reason is not editor-only is a problem",
          [name for name, _ in wrong_reason.problems] == ["a second build publishes a new generation"],
          str(wrong_reason.problems))

    near_miss = compare([line.replace("skip -- editor only", "skip -- editor only-ish")
                         if "second build" in line else line for line in EXPORTED_RUN])
    check("and `editor only` has to be the whole reason or its prefix before a colon",
          len(near_miss.problems) == 1, str(near_miss.problems))

    threadless = [line.replace("the worker task ran: ok", "the worker task ran: skip -- " + tr.NO_THREADS_WHY)
                  for line in EXPORTED_RUN]
    check("the nothreads skip is refused where it is not allowed", len(compare(threadless).problems) == 1)
    check("and accepted where it is", not compare(threadless, (tr.NO_THREADS_WHY,)).problems)

    unsectioned = compare([line for line in EXPORTED_RUN if "hover tooltip" not in line])
    check("a section's cases are missing when its one skip is",
          sorted(name for name, _ in unsectioned.problems)
          == ["a parameter is a Local Constant, not a Local Variable", "hover_probe.verse answers hovers"],
          str(unsectioned.problems))

    failing = compare([line.replace("load marshal.verse: ok", "load marshal.verse: FAIL (nothing loaded)")
                       for line in EXPORTED_RUN])
    check("a failing exported case is reported as a failure, not as a missing one",
          failing.failed == ["load marshal.verse"] and not failing.problems)


def main() -> None:
    test_parse()
    test_duplicates()
    test_comparison()
    print(f"[test_records] {'all checks passed' if FAILURES == 0 else f'{FAILURES} FAILED'}")
    sys.exit(1 if FAILURES else 0)


if __name__ == "__main__":
    main()
