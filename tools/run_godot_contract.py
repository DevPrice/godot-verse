#!/usr/bin/env python3
"""docs/architecture-review.md item 4 step 3: a headless Godot project (tests/godot_contract) that
asserts the Godot facts the bridge relies on, run on every api_version bump. That project loads no
GDExtension at all, so it needs no host token, no UE checkout and no build; everything it asserts is
either plain GDScript behaviour or a fact read out of the *running* Godot binary and, where the fact
lives only in C++ that no script can reach, out of a Godot *source* checkout (skipped when absent,
the same way the other two steps of this layer skip on a missing UE or Godot checkout).

    python tools/run_godot_contract.py --godot <Godot binary>

Prints one `[godot_contract] <name>: ok|FAIL (...)|skip -- <reason>` line per case -- the two facts
below, then every case tests/godot_contract/test_main.gd prints on its own, then the B30 log-line
check -- and a final `[godot_contract] N passed, M failed, K skipped` reparsed from everything this
run printed (tools/test_records.py, the same parser run_tests.py itself uses), so the two can never
disagree about the count.

**profiling_info_registration_stride** (CLAUDE.md "ScriptLanguageExtensionProfilingInfo is a stride
trap"; phase-6-design.md §13.3): dumps a fresh extension_api.json from the binary under test
(`--dump-extension-api`, so this always reflects the Godot actually being tested against rather than
the pinned copy in godot-cpp/gdextension) and reads native_structures' registration string for
`ScriptLanguageExtensionProfilingInfo`. Still four fields as of every Godot this repository has
tested against, one short of the real struct -- see profiling_info_struct_field_count below. If
Epic's registration ever grows a fifth field, this case starts failing, which is exactly the signal
that the workaround in `src/` (leaving `internal_time` zero) can be retired.

**profiling_info_struct_field_count** (same citations): counts the fields of
`ScriptLanguage::ProfilingInfo` in a Godot *source* checkout's `core/object/script_language.h`, and
asserts there are five. Skipped when no checkout is at `../godot`. This is the other half of the
stride trap: the struct itself, rather than what got exposed to GDExtension.

**lookup_result_script_path_key** (by-hand-findings.md B29): the lookup result Godot's editor asks a
`ScriptLanguageExtension` for is exposed across the ABI as a plain Dictionary --
`ScriptLanguageExtension::lookup_code()` in `core/object/script_language_extension.h` reads a
`"script_path"` key out of it, the field B29 says a newer Godot renamed from `script`. Nothing in
extension_api.json documents a Dictionary's keys, so the only way to check which key today's Godot
actually reads is this source scan. Skipped when no checkout is at `../godot`.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_records  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
PROJECT = REPO / "tests" / "godot_contract"
TAG = "godot_contract"

# CLAUDE.md: "Godot's real ProfilingInfo has had five fields since 4.3; its GDREGISTER_NATIVE_STRUCT
# string still lists four". Checked verbatim rather than by field count, so a rename is caught too,
# not only a count change.
EXPECTED_PROFILING_FORMAT = "StringName signature;uint64_t call_count;uint64_t total_time;uint64_t self_time"

# by-hand-findings.md B30's own sentence -- the caller's generic message, naming only the resource
# asked for and never the one it collided with. What test_main.gd's own case cannot see, because a
# script cannot read what only reaches Godot's log.
B30_LOG_SENTENCE = "Error loading resource: 'res://cyclic/a.tres'."

_lines: list[str] = []


def _emit(line: str) -> None:
    _lines.append(line)


def _ok(name: str) -> None:
    _emit(f"[{TAG}] {name}: ok")


def _fail(name: str, detail: str) -> None:
    _emit(f"[{TAG}] {name}: FAIL ({detail})")


def _skip(name: str, why: str) -> None:
    _emit(f"[{TAG}] {name}: skip -- {why}")


def find_godot_source() -> Path | None:
    guess = REPO.parent / "godot"
    return guess if (guess / "core").is_dir() else None


def dump_extension_api(godot: Path) -> dict | None:
    """A fresh `--dump-extension-api` from the binary under test, in a scratch directory -- never
    the pinned godot-cpp/gdextension/extension_api-4-7.json, which is exactly what an api_version
    bump would leave stale."""
    with tempfile.TemporaryDirectory(prefix="godot_contract_dump_") as tmp:
        try:
            result = subprocess.run([str(godot), "--headless", "--dump-extension-api", "--quit"],
                                     cwd=tmp, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, errors="replace", timeout=120)
        except subprocess.TimeoutExpired:
            _emit(f"[{TAG}] extension_api_dump: FAIL (--dump-extension-api did not exit in 120s)")
            return None
        dump_path = Path(tmp) / "extension_api.json"
        if not dump_path.is_file():
            excerpt = result.stdout.strip().replace("\n", " / ")[:200]
            _emit(f"[{TAG}] extension_api_dump: FAIL (wrote no extension_api.json; "
                  f"exit {result.returncode}: {excerpt})")
            return None
        return json.loads(dump_path.read_text(encoding="utf-8"))


def check_profiling_info_registration(api: dict | None) -> None:
    name = "profiling_info_registration_stride"
    if api is None:
        _skip(name, "extension_api.json could not be dumped from the running binary")
        return
    for struct in api.get("native_structures", []):
        if struct["name"] == "ScriptLanguageExtensionProfilingInfo":
            got = struct["format"]
            if got == EXPECTED_PROFILING_FORMAT:
                _ok(name)
            else:
                _fail(name, f"got {got!r}, expected {EXPECTED_PROFILING_FORMAT!r}")
            return
    _fail(name, "ScriptLanguageExtensionProfilingInfo is no longer in native_structures")


_PROFILING_INFO_STRUCT = re.compile(r"struct ProfilingInfo\s*\{([^}]*)\};")


def check_profiling_info_struct(godot_src: Path | None) -> None:
    name = "profiling_info_struct_field_count"
    if godot_src is None:
        _skip(name, f"no Godot source checkout at {REPO.parent / 'godot'}")
        return
    header = godot_src / "core" / "object" / "script_language.h"
    if not header.is_file():
        _skip(name, f"{header} is missing")
        return
    text = header.read_text(encoding="utf-8")
    match = _PROFILING_INFO_STRUCT.search(text)
    if match is None:
        _fail(name, f"struct ProfilingInfo not found in {header}")
        return
    fields = [line.strip() for line in match.group(1).splitlines() if line.strip().endswith(";")]
    if len(fields) == 5:
        _ok(name)
    else:
        _fail(name, f"got {len(fields)} fields ({'; '.join(fields)}), expected 5")


_LOOKUP_SCRIPT_PATH_KEY = re.compile(r'r_result\.script_path\s*=\s*ret\.get\(\s*"script_path"')


def check_lookup_result_script_path(godot_src: Path | None) -> None:
    name = "lookup_result_script_path_key"
    if godot_src is None:
        _skip(name, f"no Godot source checkout at {REPO.parent / 'godot'}")
        return
    header = godot_src / "core" / "object" / "script_language_extension.h"
    if not header.is_file():
        _skip(name, f"{header} is missing")
        return
    text = header.read_text(encoding="utf-8")
    if _LOOKUP_SCRIPT_PATH_KEY.search(text):
        _ok(name)
    else:
        _fail(name, f"lookup_code() in {header} no longer reads a \"script_path\" dictionary key "
                     "-- see by-hand-findings.md B29")


def run_project(godot: Path) -> tuple[int, str]:
    argv = [str(godot), "--headless", "--path", str(PROJECT), "--script", "res://test_main.gd",
            "--quit-after", "60"]
    try:
        result = subprocess.run(argv, cwd=str(REPO), stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, errors="replace", timeout=120)
        return result.returncode, result.stdout
    except subprocess.TimeoutExpired as timeout:
        return 1, (timeout.stdout or "") + "\n[godot_contract] the project did not exit in 120s\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--godot", required=True, help="the Godot binary")
    args = parser.parse_args()

    godot = Path(args.godot)
    if not godot.is_file():
        print(f"[{TAG}] {godot} is not a file", file=sys.stderr)
        return 2
    if not (PROJECT / "project.godot").is_file():
        print(f"[{TAG}] {PROJECT} is not a Godot project", file=sys.stderr)
        return 2

    godot_src = find_godot_source()

    check_profiling_info_registration(dump_extension_api(godot))
    check_profiling_info_struct(godot_src)
    check_lookup_result_script_path(godot_src)

    returncode, output = run_project(godot)
    _emit(output.rstrip("\n"))
    if "passed, " not in output:
        _emit(f"[{TAG}] the project exited without printing its own summary line -- it stopped early")

    if B30_LOG_SENTENCE in output:
        _ok("b30_cyclic_load_log_sentence")
    else:
        _fail("b30_cyclic_load_log_sentence", f"{B30_LOG_SENTENCE!r} never reached the output")

    combined = "\n".join(_lines)
    cases = test_records.parse_cases(combined, "contract", TAG, TAG)
    passed = sum(1 for case in cases if case.status == test_records.PASS)
    failed = sum(1 for case in cases if case.status == test_records.FAIL)
    skipped = sum(1 for case in cases if case.status == test_records.SKIP)
    if returncode != 0 and failed == 0:
        # Every case the project or this script itself reported failing is already counted above,
        # so a nonzero exit with no case explaining it is the process dying outright -- a crash,
        # not a reported FAIL.
        _emit(f"[{TAG}] tests/godot_contract exited {returncode} with no case explaining why")
        failed += 1

    print(combined)
    print(f"[{TAG}] {passed} passed, {failed} failed, {skipped} skipped")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
