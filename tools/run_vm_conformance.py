#!/usr/bin/env python3
"""The differential conformance harness (design phase-7.5 §10.2, task T3.0).

Cooks every fixture in tests/vm_conformance/*.verse as one project, then runs
tests/cooked_probe's binary against a class at a time, once per host, and diffs the transcripts
line by line (method by method) after normalizing what legitimately differs between two DLLs
(the host-kind line, the probe's own DLL-load line, addresses).

    python tools/run_vm_conformance.py --record       # cook, run against the UE runtime host,
                                                        # write tests/vm_conformance/expected/*.txt
    python tools/run_vm_conformance.py                 # cook cached from --record; run bin/verse_vm.dll
                                                        # against the recorded expected/*.txt; no UE
                                                        # checkout needed
    python tools/run_vm_conformance.py --sequential     # skip fixtures marked concurrent (tasks_*.verse)
    python tools/run_vm_conformance.py --only tasks     # only classes whose name contains "tasks"

A fixture file's class is its own name (values_ints.verse declares class values_ints), and every
fixture in this directory is cooked as one project, so the class list is the file list. A file
named tasks_*.verse is "concurrent" and is skipped under --sequential.

UE_ROOT (or --engine) is only read by --record, which needs verse_cook.exe and
verse_host_runtime.dll out of the checkout. Default mode reads only the cache --record filled and
bin/verse_vm.dll; it never touches the checkout.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FIXTURES_DIR = REPO / "tests" / "vm_conformance"
EXPECTED_DIR = FIXTURES_DIR / "expected"
COOK_CACHE_DIR = REPO / "scratch" / "vm_conformance_cook"
COOKED_PROBE = REPO / "bin" / "cooked_probe.exe"
VM_DLL = REPO / "bin" / "verse_vm.dll"

CONCURRENT_PREFIX = "tasks_"


def find_engine(explicit: str | None) -> Path | None:
    for candidate in (explicit, os.environ.get("UE_ROOT")):
        if candidate:
            path = Path(candidate)
            return path if path.is_dir() else None
    guess = REPO.parent / "UnrealEngine"
    return guess if guess.is_dir() else None


def fixture_files() -> list[Path]:
    return sorted(p for p in FIXTURES_DIR.glob("*.verse") if p.is_file())


def is_concurrent(class_name: str) -> bool:
    return class_name.startswith(CONCURRENT_PREFIX)


# --------------------------------------------------------------------------------- normalizing --

_HOST_KIND_RE = re.compile(r"^\[probe\] vh_host_kind = -?\d+$")
_GETLASTERROR_RE = re.compile(r"^\[probe\] GetLastError=\d+$")
_POINTER_RE = re.compile(r"0x[0-9a-fA-F]+")


def normalize(text: str) -> str:
    """Strips what legitimately differs between two DLLs and nothing else.

    vh_host_kind answers a different constant for a runtime host and for this interpreter
    (design §9's `verse/runtime/backend`); GetLastError is a Windows code that means nothing once
    LoadLibraryW already failed one way or the other; a bare pointer value never repeats between
    two different processes. A `LogVerseRuntime:` line is the UE host's engine log echoing a
    runtime error that the `[error]` lines already carry; the interpreter has no engine log.
    """
    out_lines = []
    for line in text.splitlines():
        if line.startswith("LogVerseRuntime:"):
            continue
        if _HOST_KIND_RE.match(line):
            line = "[probe] vh_host_kind = <normalized>"
        elif _GETLASTERROR_RE.match(line):
            line = "[probe] GetLastError=<normalized>"
        line = _POINTER_RE.sub("<ptr>", line)
        out_lines.append(line)
    return "\n".join(out_lines) + ("\n" if text.endswith("\n") else "")


# ----------------------------------------------------------------------------------- transcript --

class MethodBlock:
    def __init__(self, name: str, lines: list[str]):
        self.name = name
        self.lines = lines


def parse_transcript(text: str) -> tuple[list[str], dict[str, MethodBlock]]:
    """Splits a cooked_probe transcript into the preamble (before the first call) and one block
    per "[probe] calling X" .. next such line / [probe] done, in declaration order.
    """
    lines = text.splitlines()
    calling_re = re.compile(r"^\[probe\] calling (.+)$")
    starts: list[tuple[int, str]] = []
    for index, line in enumerate(lines):
        match = calling_re.match(line)
        if match:
            starts.append((index, match.group(1)))

    preamble_end = starts[0][0] if starts else len(lines)
    preamble = lines[:preamble_end]

    blocks: dict[str, MethodBlock] = {}
    for i, (start, name) in enumerate(starts):
        end = starts[i + 1][0] if i + 1 < len(starts) else len(lines)
        blocks[name] = MethodBlock(name, lines[start:end])
    return preamble, blocks


# --------------------------------------------------------------------------------- running a dll --

def run_cooked_probe(dll: Path, cooked_dir: Path, class_name: str) -> tuple[int, str]:
    argv = [str(COOKED_PROBE), str(dll), str(cooked_dir), str(cooked_dir / "Cooked"), class_name]
    completed = subprocess.run(argv, capture_output=True, text=True, errors="replace")
    return completed.returncode, normalize((completed.stdout or "") + (completed.stderr or ""))


# ------------------------------------------------------------------------------------- recording --

def build_manifest(work: Path) -> Path:
    sources = fixture_files()
    manifest = work / "sources.txt"
    manifest.write_text(
        "".join(f"{path}\t\tres://{path.name}\n" for path in sources), encoding="utf-8")
    return manifest


def do_record(engine: Path, only: str | None) -> int:
    cooker = engine / "Engine" / "Binaries" / "Win64" / "verse_cook.exe"
    if not cooker.is_file():
        print(f"[run_vm_conformance] {cooker} not built -- run tools/build_host.py "
              "--target VerseHostCooker")
        return 1
    runtime_host = engine / "Engine" / "Binaries" / "Win64" / "verse_host_runtime.dll"
    if not runtime_host.is_file():
        print(f"[run_vm_conformance] {runtime_host} not built -- run tools/build_host.py "
              "--target VerseHostRuntime")
        return 1
    if not COOKED_PROBE.is_file():
        print(f"[run_vm_conformance] {COOKED_PROBE} not built -- run tools/build_cooked_probe.py")
        return 1

    sources = fixture_files()
    if not sources:
        print(f"[run_vm_conformance] no fixtures under {FIXTURES_DIR}")
        return 1

    if COOK_CACHE_DIR.exists():
        shutil.rmtree(COOK_CACHE_DIR)
    COOK_CACHE_DIR.mkdir(parents=True)

    with tempfile.TemporaryDirectory(prefix="vm_conformance_manifest_") as work_str:
        manifest = build_manifest(Path(work_str))
        print(f"[run_vm_conformance] cooking {len(sources)} fixture(s) -> {COOK_CACHE_DIR}")
        completed = subprocess.run([str(cooker), str(manifest), str(COOK_CACHE_DIR)],
                                   capture_output=True, text=True, errors="replace")
        sys.stdout.write(completed.stdout or "")
        if completed.returncode != 0:
            sys.stdout.write(completed.stderr or "")
            print(f"[run_vm_conformance] verse_cook.exe exited {completed.returncode}: FAIL")
            return 1

    EXPECTED_DIR.mkdir(parents=True, exist_ok=True)
    classes = [path.stem for path in sources]
    if only:
        classes = [c for c in classes if only in c]

    total_methods = 0
    for class_name in classes:
        returncode, transcript = run_cooked_probe(runtime_host, COOK_CACHE_DIR, class_name)
        _, blocks = parse_transcript(transcript)
        total_methods += len(blocks)
        (EXPECTED_DIR / f"{class_name}.txt").write_text(transcript, encoding="utf-8")
        print(f"[run_vm_conformance] recorded {class_name}: {len(blocks)} method(s), "
              f"probe exit {returncode}")

    print(f"[run_vm_conformance] recorded {len(classes)} class(es), {total_methods} method(s) total")
    return 0


# ------------------------------------------------------------------------------------- comparing --

def diff_blocks(expected: MethodBlock, actual: MethodBlock | None, preamble: list[str]) -> str | None:
    """None on agreement; otherwise the first differing line, for the one-line report."""
    if actual is None:
        header = preamble[-1] if preamble else "(no output)"
        return f"vm produced no output for this call -- {header}"
    for i in range(max(len(expected.lines), len(actual.lines))):
        exp_line = expected.lines[i] if i < len(expected.lines) else "(missing)"
        act_line = actual.lines[i] if i < len(actual.lines) else "(missing)"
        if exp_line != act_line:
            return f"expected {exp_line!r}, got {act_line!r}"
    return None


def do_compare(only: str | None, sequential: bool) -> int:
    if not EXPECTED_DIR.is_dir() or not any(EXPECTED_DIR.glob("*.txt")):
        print(f"[run_vm_conformance] no recorded transcripts under {EXPECTED_DIR} -- run "
              "tools/run_vm_conformance.py --record first (needs a UE checkout)")
        return 1
    if not VM_DLL.is_file():
        print(f"[run_vm_conformance] {VM_DLL} not built -- run tools/build_verse_vm.py")
        return 1
    if not COOKED_PROBE.is_file():
        print(f"[run_vm_conformance] {COOKED_PROBE} not built -- run tools/build_cooked_probe.py")
        return 1
    if not COOK_CACHE_DIR.is_dir():
        print(f"[run_vm_conformance] no cook cached at {COOK_CACHE_DIR} -- run "
              "tools/run_vm_conformance.py --record first (needs a UE checkout)")
        return 1

    classes = sorted(path.stem for path in EXPECTED_DIR.glob("*.txt"))
    if only:
        classes = [c for c in classes if only in c]

    ok_count = 0
    diff_count = 0
    skip_count = 0

    for class_name in classes:
        if sequential and is_concurrent(class_name):
            print(f"[run_vm_conformance] {class_name}: SKIP -- concurrent, --sequential given")
            skip_count += 1
            continue

        expected_text = normalize((EXPECTED_DIR / f"{class_name}.txt").read_text(encoding="utf-8"))
        _expected_preamble, expected_blocks = parse_transcript(expected_text)
        _, actual_transcript = run_cooked_probe(VM_DLL, COOK_CACHE_DIR, class_name)
        actual_preamble, actual_blocks = parse_transcript(actual_transcript)

        for method_name, expected_block in expected_blocks.items():
            actual_block = actual_blocks.get(method_name)
            reason = diff_blocks(expected_block, actual_block, actual_preamble)
            full_name = f"{class_name}.{method_name}"
            if reason is None:
                print(f"[run_vm_conformance] {full_name}: ok")
                ok_count += 1
            else:
                print(f"[run_vm_conformance] {full_name}: DIFF -- {reason}")
                diff_count += 1

        extra = set(actual_blocks) - set(expected_blocks)
        for method_name in sorted(extra):
            print(f"[run_vm_conformance] {class_name}.{method_name}: DIFF -- vm called a method "
                  "the recorded transcript never called")
            diff_count += 1

    print(f"[run_vm_conformance] {ok_count} ok, {diff_count} DIFF, {skip_count} class(es) skipped")
    return 0 if diff_count == 0 else 1


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--record", action="store_true",
                         help="cook and run against the UE runtime host; write expected/*.txt")
    parser.add_argument("--sequential", action="store_true",
                         help="skip fixtures marked concurrent (tasks_*.verse)")
    parser.add_argument("--only", help="only classes whose name contains this substring")
    parser.add_argument("--engine", help="the Unreal checkout (default: UE_ROOT, then ../UnrealEngine); "
                                          "--record only")
    args = parser.parse_args()

    if args.record:
        engine = find_engine(args.engine)
        if engine is None:
            print("[run_vm_conformance] no Unreal checkout -- set UE_ROOT or pass --engine")
            sys.exit(1)
        sys.exit(do_record(engine, args.only))
    else:
        sys.exit(do_compare(args.only, args.sequential))


if __name__ == "__main__":
    main()
