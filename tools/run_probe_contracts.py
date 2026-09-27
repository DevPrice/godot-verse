#!/usr/bin/env python3
"""Runs every tests/verse_probe fixture and holds its transcript to a recorded golden.

docs/architecture-review.md item 4 step 1: `verse_probe` measures what the Verse compiler and the
VM actually do -- 68 of 77 fixtures with `--class`, a class instantiated and its zero-argument
methods called; the other 9 compile-only, most of them `*_reject.verse` files whose diagnostic
*text* is the answer -- but it asserts nothing itself and nothing re-runs it when the engine moves.
This is what promotes each fixture into an assertion: a golden transcript under
tests/verse_probe/expected/, and a byte-for-byte (after normalizing what cannot be the same twice)
comparison against a fresh run. Two fixtures are excluded rather than recorded, because what they
measure is genuinely nondeterministic from run to run and no normalization makes it otherwise --
see their FIXTURES rows below.

    python tools/run_probe_contracts.py --engine <UE checkout>            # check (the default)
    python tools/run_probe_contracts.py --engine <UE checkout> --record   # (re)write the goldens

Each fixture is one flat `bin/verse_probe.exe` invocation -- the file, and `--class <name>` when the
header gives one, taken from each fixture's own header comment. FIXTURES below is that table; a
fixture the header does not spell out an invocation for is marked `# invented` at its row, with the
reasoning in this file's history and in the orchestrator's report. `vh_init` is one call per process
(CLAUDE.md: "one attempt per process"), so every fixture is its own subprocess -- there is no way to
batch them into one host session.

A golden is normalized before it is written and before it is compared, the same way both times:
absolute paths under the repo and the engine checkout become `<repo>`/`<engine>` (a worktree's path
and another machine's checkout path are both otherwise baked into a diagnostic's `FilePathUtf8`,
which crosses the ABI verbatim), a hex address becomes `<addr>` (ASLR; only `vm_values_false_probe`,
the one fixture whose fatal assertion prints a callstack, has any), and `generation N` becomes
`generation <gen>` (always 1 in a fresh process today, but the fact that it is not printed as a raw
number is what makes that not load-bearing on this check).
"""

from __future__ import annotations

import argparse
import difflib
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FIXTURES_DIR = REPO / "tests" / "verse_probe"
EXPECTED_DIR = FIXTURES_DIR / "expected"
PROBE_EXE = REPO / "bin" / "verse_probe.exe"


@dataclass(frozen=True)
class Fixture:
    name: str
    # None when the fixture's own header invocation carries no --class -- every *_reject.verse
    # file (compile fails, so no class is ever instantiated) and virtual_spawn.verse (whose header
    # invocation is compile-only; every method is <suspends> or a Godot virtual override, both
    # skipped by the driver's call loop, so --class would print a method list and call nothing).
    class_name: str | None
    # Set only for a fixture whose header gives no invocation at all; the class name was worked
    # out from the file's own declarations rather than transcribed. All match the fixture's own
    # file-named class, which is the only one CLAUDE.md's "class named after the file" rule lets a
    # node hold -- the same name the ABI's vh_has_class/vh_instantiate take.
    invented: bool = False
    # A reason to skip this fixture rather than record or check it -- for output this checker
    # cannot normalize into something stable, not for a fixture that merely fails today.
    exclude: str | None = None


FIXTURES: list[Fixture] = [
    Fixture("async_probe.verse", "async_probe"),
    Fixture("async_reject.verse", None),
    Fixture("attribute_name_probe.verse", "attribute_name_probe"),
    Fixture("call_const_probe.verse", "call_const_probe"),
    Fixture("class_block_probe.verse", "class_block_probe"),
    Fixture("class_block_self_probe.verse", "class_block_self_probe"),
    Fixture("construct_probe.verse", "construct_probe"),
    Fixture("ctor_delegate_probe.verse", "ctor_delegate_probe"),
    Fixture("decides_virtual_probe.verse", "decides_virtual_probe"),
    Fixture("decides_virtual_reject.verse", None),
    Fixture("default_cdo_probe.verse", "default_cdo_probe"),
    Fixture("doc_attribute_probe.verse", "doc_attribute_probe", invented=True),
    Fixture("effect_trap.verse", None),
    Fixture("effects_probe.verse", "effects_probe"),
    Fixture("effects_reject.verse", None),
    Fixture("effects_variance.verse", "effects_variance", invented=True),
    Fixture("empty_class_probe.verse", "probe_main"),
    Fixture("event_probe.verse", "event_probe"),
    Fixture("event_resume.verse", "event_resume"),
    Fixture("example.verse", "example"),
    Fixture("export_signal_probe.verse", "export_signal_probe"),
    Fixture("final_probe.verse", "final_probe"),
    Fixture("final_reject.verse", None),
    Fixture("hooks_probe.verse", "hooks_probe"),
    Fixture("math_facts.verse", "math_facts"),
    Fixture("narrow_suspends.verse", "narrow_suspends"),
    Fixture("native_block_probe.verse", "native_block_probe"),
    Fixture("option_overload_probe.verse", "probe_main"),
    Fixture("option_param_probe.verse", "probe_main"),
    Fixture("option_variance_probe.verse", "probe_main"),
    Fixture("overload_probe.verse", "overload_probe"),
    Fixture("profile_probe.verse", "profile_probe"),
    Fixture("reads_property.verse", "reads_property", invented=True),
    Fixture("ref_block_probe.verse", "ref_block_probe"),
    Fixture("rid_probe.verse", "rid_probe"),
    Fixture("rpc_attribute_probe.verse", "rpc_attribute_probe"),
    Fixture("signal_access_probe.verse", "signal_access_probe"),
    Fixture("signal_listenable_probe.verse", "signal_listenable_probe"),
    Fixture("signal_shadow_probe.verse", "signal_shadow_probe"),
    Fixture("singleton_effect_probe.verse", "singleton_effect_probe", invented=True),
    Fixture("sleep_probe.verse", "sleep_probe"),
    Fixture("stdlib_types_probe.verse", "stdlib_types_probe", invented=True),
    Fixture("subscribable_event_probe.verse", "subscribable_event_probe"),
    Fixture("tostring_probe.verse", "tostring_probe", invented=True),
    Fixture("vararg_arity_probe.verse", "vararg_arity_probe"),
    Fixture("vararg_probe.verse", "vararg_probe"),
    Fixture("variant_any_probe.verse", "variant_any_probe"),
    Fixture("variant_api_probe.verse", "variant_api_probe"),
    Fixture("virtual_spawn.verse", None),
    Fixture("vm_calls_probe.verse", "vm_calls_probe"),
    Fixture("vm_facts_probe.verse", "vm_facts_probe"),
    Fixture("vm_failure_probe.verse", "vm_failure_probe"),
    Fixture("vm_failure_reject.verse", None),
    Fixture("vm_modules_probe.verse", "vm_modules_probe"),
    Fixture("vm_modules_var_reject.verse", "vm_modules_var_reject"),
    Fixture("vm_modules_varfield_probe.verse", "vm_modules_varfield_probe"),
    Fixture("vm_natives_edges_probe.verse", "vm_natives_edges_probe"),
    Fixture("vm_natives_probe.verse", "vm_natives_probe"),
    Fixture("vm_natives_reach_reject.verse", "vm_natives_reach_reject"),
    Fixture("vm_objects_global_probe.verse", "vm_objects_global_probe"),
    Fixture("vm_objects_probe.verse", "vm_objects_probe"),
    Fixture("vm_objects_reject.verse", "vm_objects_reject"),
    Fixture("vm_objects_wideint_probe.verse", "vm_objects_wideint_probe"),
    Fixture("vm_ops_live_probe.verse", "vm_ops_live_probe"),
    Fixture("vm_ops_probe.verse", "vm_ops_probe"),
    Fixture("vm_tasks_probe.verse", "vm_tasks_probe"),
    Fixture("vm_tasks_probe2.verse", "vm_tasks_probe2"),
    Fixture("vm_tasks_probe3.verse", "vm_tasks_probe3"),
    Fixture("vm_tasks_reject.verse", None),
    # Measured (three runs, no two alike) to deliver a signal to its several subscribers in a
    # different order each process: sub 1..6 interleave differently in "sub N got 1"/"got 2" every
    # time, which is a real fact about this VM (nothing orders concurrent Subscribe handlers) and
    # not something a byte-for-byte transcript can hold without hiding the header's own claim behind
    # a fragile diff. Excluded rather than normalized: there is no stable substring to reorder onto.
    Fixture("vm_tasks_subscribe_probe.verse", "vm_tasks_subscribe_probe",
            exclude="subscriber delivery order is not deterministic across runs (measured: three "
                    "runs of the unchanged binary, no two orderings alike)"),
    Fixture("vm_unification_probe.verse", "vm_unification_probe"),
    Fixture("vm_unification_reject.verse", None),
    # Measured (twice, diffed after normalizing) to abort the process with the same fatal
    # assertion and callstack every time -- the header's own claim ("prints nothing... and
    # nothing else runs after it") is itself the contract, so this is recorded rather than
    # excluded. Only its hex addresses are unstable (ASLR); everything else, including the
    # Microsoft CRT's own build path baked into its PDB, held identical across two runs.
    Fixture("vm_values_false_probe.verse", "vm_values_false_probe", invented=True),
    Fixture("vm_values_native_struct_probe.verse", "vm_values_native_struct_probe", invented=True),
    # Measured (three runs, unchanged binary) to answer CrossKind's false-string/empty-string map
    # lookup differently from run to run -- "x: false-string finds empty-string key" flips between
    # "= empty-key" and "fails", and its mirror image with it, while the other ~380 lines this
    # fixture prints held identical across the same three runs. A real finding (whether `false`
    # standing in for an empty string hashes equal to `""` is not stable), and exactly the kind
    # item 4 exists to surface, but not one two lines of normalization should paper over -- excluded
    # rather than weakened.
    Fixture("vm_values_probe.verse", "vm_values_probe", invented=True,
            exclude="CrossKind's false-string/empty-string map key lookup is not deterministic "
                    "across runs (measured: three runs of the unchanged binary, two outcomes seen)"),
    Fixture("vm_values_reject.verse", "vm_values_reject", invented=True),
    Fixture("wide_callback.verse", "wide_callback"),
]


def find_engine(explicit: str | None) -> Path | None:
    for candidate in (explicit, os.environ.get("UE_ROOT")):
        if candidate:
            path = Path(candidate)
            return path if path.is_dir() else None
    guess = REPO.parent / "UnrealEngine"
    return guess if guess.is_dir() else None


_ADDR = re.compile(r"0x[0-9a-fA-F]+")
_GENERATION = re.compile(r"generation \d+")


def normalize(text: str, repo: Path, engine: Path) -> str:
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    for base, token in ((str(repo), "<repo>"), (str(engine), "<engine>")):
        text = text.replace(base, token)
        text = text.replace(base.replace("\\", "/"), token)
    text = _ADDR.sub("<addr>", text)
    text = _GENERATION.sub("generation <gen>", text)
    return text


def run_fixture(fixture: Fixture, engine: Path) -> str:
    dll = engine / "Engine" / "Binaries" / "Win64" / "verse_host.dll"
    engine_dir = engine / "Engine"
    fixture_path = FIXTURES_DIR / fixture.name
    argv = [str(PROBE_EXE), str(dll), str(engine_dir), str(fixture_path)]
    if fixture.class_name is not None:
        argv += ["--class", fixture.class_name]
    result = subprocess.run(argv, cwd=str(PROBE_EXE.parent), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace", timeout=120)
    transcript = normalize(result.stdout, REPO, engine)
    return transcript + f"[probe_contract] exit code: {result.returncode}\n"


def golden_path(fixture: Fixture) -> Path:
    return EXPECTED_DIR / f"{fixture.name}.txt"


def record(engine: Path) -> int:
    EXPECTED_DIR.mkdir(parents=True, exist_ok=True)
    for fixture in FIXTURES:
        if fixture.exclude:
            print(f"[probe_contract] {fixture.name}: excluded -- {fixture.exclude}")
            continue
        transcript = run_fixture(fixture, engine)
        golden_path(fixture).write_text(transcript, encoding="utf-8", newline="\n")
        print(f"[probe_contract] {fixture.name}: recorded")
    return 0


def check(engine: Path) -> int:
    failed = 0
    skipped = 0
    for fixture in FIXTURES:
        if fixture.exclude:
            skipped += 1
            print(f"[probe_contract] {fixture.name}: skip -- {fixture.exclude}")
            continue
        path = golden_path(fixture)
        if not path.is_file():
            failed += 1
            print(f"[probe_contract] {fixture.name}: FAIL (no golden at {path} -- run --record)")
            continue
        expected = path.read_text(encoding="utf-8")
        actual = run_fixture(fixture, engine)
        if actual == expected:
            print(f"[probe_contract] {fixture.name}: ok")
        else:
            failed += 1
            diff = list(difflib.unified_diff(expected.splitlines(keepends=True),
                                             actual.splitlines(keepends=True),
                                             fromfile="expected", tofile="actual"))
            excerpt = "".join(diff[:12]).replace("\n", " / ")
            print(f"[probe_contract] {fixture.name}: FAIL ({excerpt})")
    total = len(FIXTURES) - skipped
    print(f"[probe_contract] {total - failed} passed, {failed} failed, {skipped} skipped")
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--engine", help="the Unreal checkout (default: UE_ROOT, then ../UnrealEngine)")
    parser.add_argument("--record", action="store_true", help="(re)write every golden instead of checking it")
    args = parser.parse_args()

    engine = find_engine(args.engine)
    if engine is None:
        print("[probe_contract] no Unreal checkout -- set UE_ROOT or pass --engine", file=sys.stderr)
        return 2
    if not PROBE_EXE.is_file():
        print(f"[probe_contract] {PROBE_EXE} not built -- run tools/build_verse_probe.py", file=sys.stderr)
        return 2

    return record(engine) if args.record else check(engine)


if __name__ == "__main__":
    sys.exit(main())
