"""include/verse_diagnostics.def: the registry of bridge-authored diagnostics, held to its users.

A diagnostic is asserted by its ID, so the ID is only worth something while it names exactly one
sentence and every ID anything prints or asserts is one the registry defines. This reads the
registry the way the preprocessor does -- one VERSE_DIAG(id, where, text) per row, text as adjacent
string literals -- and holds it to src/, vm/ and host/ (which emit the IDs), to the tests and tools that
assert them, and to docs/diagnostics.md, which is the page an author lands on from one.

One line per case, exit status 1 on any failure.
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
REGISTRY = REPO / "include" / "verse_diagnostics.def"
DOC = REPO / "docs" / "diagnostics.md"

ID = re.compile(r"\bVG\d{4}\b")
LITERAL = r'"(?:[^"\\\n]|\\.)*"'
ROW = re.compile(rf"VERSE_DIAG\(\s*(VG\d{{4}})\s*,\s*((?:{LITERAL}\s*)+),\s*((?:{LITERAL}\s*)+)\)")
# The block an ID's thousands digit puts it in (docs/diagnostics.md, "How IDs are numbered").
BLOCKS = {"1", "2", "3", "4", "5", "6", "7"}

FAILURES = 0


def check(name: str, ok: bool, detail: str = "") -> None:
    global FAILURES
    if ok:
        print(f"[verse_diagnostics] {name}: ok")
    else:
        FAILURES += 1
        print(f"[verse_diagnostics] {name}: FAIL" + (f" ({detail})" if detail else ""))


def unquote(literals: str) -> str:
    """Adjacent C string literals, joined. The registry uses no escape but \\" and \\\\."""
    parts = re.findall(LITERAL, literals)
    return "".join(part[1:-1].replace('\\"', '"').replace("\\\\", "\\") for part in parts)


def read_registry() -> list[tuple[str, str, str]]:
    code = "\n".join(line for line in REGISTRY.read_text(encoding="utf-8").splitlines()
                     if not line.lstrip().startswith("//"))
    rows = [(m.group(1), unquote(m.group(2)), unquote(m.group(3))) for m in ROW.finditer(code)]
    # Every VERSE_DIAG( the preprocessor would see has to be one the regex read, or a row with a
    # typo in it would be compiled and never checked.
    check("every VERSE_DIAG row in the registry is well formed",
          len(rows) == code.count("VERSE_DIAG("), f"{code.count('VERSE_DIAG(')} rows, {len(rows)} parsed")
    return rows


def files_under(*roots: str, suffixes: tuple[str, ...]) -> list[Path]:
    found: list[Path] = []
    for root in roots:
        base = REPO / root
        if base.is_file():
            found.append(base)
            continue
        found.extend(path for path in base.rglob("*") if path.suffix in suffixes and path.is_file())
    return found


def ids_in(paths: list[Path]) -> dict[str, list[str]]:
    seen: dict[str, list[str]] = {}
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        for found in ID.findall(text):
            seen.setdefault(found, []).append(str(path.relative_to(REPO)).replace("\\", "/"))
    return seen


def main() -> None:
    rows = read_registry()
    ids = [row[0] for row in rows]
    registered = set(ids)

    duplicate_ids = sorted({i for i in ids if ids.count(i) > 1})
    check("every ID in the registry is unique", not duplicate_ids, ", ".join(duplicate_ids))

    texts: dict[str, str] = {}
    shared = []
    for row_id, _, text in rows:
        if text in texts:
            shared.append(f"{texts[text]} and {row_id}")
        texts.setdefault(text, row_id)
    check("no two IDs share a template", not shared, "; ".join(shared))

    misplaced = [i for i in ids if i[2] not in BLOCKS]
    check("every ID is in one of the documented blocks", not misplaced, ", ".join(misplaced))

    unnamed = [row_id for row_id, where, _ in rows if not where.strip()]
    check("every row says where it is emitted", not unnamed, ", ".join(unnamed))

    empty = [row_id for row_id, _, text in rows if not text.strip()]
    check("every row has a sentence", not empty, ", ".join(empty))

    # The registry itself is the one place an ID may appear without being used.
    source = [p for p in files_under("src", "vm", "host/Private", "include", suffixes=(".cpp", ".h"))
              if p.name != "verse_diagnostics.def"]
    emitted = ids_in(source)
    unknown = sorted(i for i in emitted if i not in registered)
    check("every ID src/, vm/ and host/ emit is in the registry", not unknown,
          ", ".join(f"{i} in {emitted[i][0]}" for i in unknown))
    unused = sorted(i for i in registered if i not in emitted)
    check("every ID in the registry is emitted somewhere", not unused, ", ".join(unused))

    asserted = ids_in(files_under("tools", "tests", suffixes=(".py", ".gd", ".cpp", ".h")))
    unknown = sorted(i for i in asserted if i not in registered)
    check("every ID a test asserts is in the registry", not unknown,
          ", ".join(f"{i} in {asserted[i][0]}" for i in unknown))

    doc_ids = ids_in([DOC]) if DOC.is_file() else {}
    check("docs/diagnostics.md exists", DOC.is_file())
    missing = sorted(i for i in registered if i not in doc_ids)
    check("docs/diagnostics.md documents every registered ID", not missing, ", ".join(missing))
    stale = sorted(i for i in doc_ids if i not in registered)
    check("docs/diagnostics.md documents no ID the registry lacks", not stale, ", ".join(stale))

    print(f"[verse_diagnostics] {len(rows)} diagnostics registered; "
          f"{'all checks passed' if FAILURES == 0 else f'{FAILURES} FAILED'}")
    sys.exit(1 if FAILURES else 0)


if __name__ == "__main__":
    main()
