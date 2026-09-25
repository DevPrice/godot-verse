"""Checks that docs/web-vm/spec/ops.md documents every op ops.json says the compiler emits.

    python tools/check_spec.py

One line per missing op, exit 1 if any. An op counts as documented when ops.md has a heading
`### <OpName>`. Also reports every spec file whose Status line is not "reviewed", since the clean
room may not read those.
"""

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import gen_vbc_ops  # noqa: E402

SPEC = REPO / "docs" / "web-vm" / "spec"


def main():
    failures = 0
    ops_md = SPEC / "ops.md"
    if not ops_md.exists():
        print(f"FAIL {ops_md} does not exist")
        return 1
    headings = set(re.findall(r"^### (\w+)\s*$", ops_md.read_text(encoding="utf-8"), re.MULTILINE))
    emitted = [op["name"] for op in gen_vbc_ops.load()["ops"] if op["emitted"]]
    for name in emitted:
        if name not in headings:
            print(f"FAIL ops.md has no ### {name}")
            failures += 1

    for path in sorted(SPEC.glob("*.md")):
        status = next((line for line in path.read_text(encoding="utf-8").splitlines()
                       if line.startswith("Status:")), "")
        if "reviewed" not in status:
            print(f"FAIL {path.name} is not reviewed: {status or 'no Status line'}")
            failures += 1

    if failures:
        return 1
    print(f"ok {len(emitted)} emitted ops documented; every spec file reviewed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
