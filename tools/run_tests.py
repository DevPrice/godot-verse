#!/usr/bin/env python3
"""Runs every test a contributor can run locally, and reports pass/fail without interpretation.

R-QUAL-3. The three layers R-QUAL-1 names, in the order a failure is cheapest to read:

  units        the lexer and the class-declaration scanner, which need neither Godot nor UE
  abi          host_smoke, which drives the whole C ABI with no Godot
  integration  a headless Godot with Verse scripts attached, asserting on behaviour

Each layer is skipped rather than failed when what it needs is absent -- a contributor without a UE
checkout still gets the unit layer -- and a skip is reported as a skip, never as a pass.

  python tools/run_tests.py                 # everything that can run here
  python tools/run_tests.py --only units    # one layer
  python tools/run_tests.py --build         # rebuild the test binaries first

Environment: UE_ROOT names the Unreal checkout (or --engine), GODOT names the Godot binary (or
--godot). Both are also guessed from the usual places.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Where a Godot binary tends to be when nobody has said. Deliberately short: a guess that finds the
# wrong Godot is worse than no guess, because the tests would then report on a build nobody meant.
GODOT_GUESSES = [
    Path(r"C:\Apps\Godot_v4.7-stable_win64.exe\Godot_v4.7-stable_win64_console.exe"),
    Path(r"C:\Apps\Godot_v4.7-stable_win64.exe\Godot_v4.7-stable_win64.exe"),
]


class Results:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0
        self.skipped: list[str] = []

    def record(self, name: str, ok: bool) -> None:
        if ok:
            self.passed += 1
            print(f"[run_tests] {name}: PASS")
        else:
            self.failed += 1
            print(f"[run_tests] {name}: FAIL")

    def skip(self, name: str, why: str) -> None:
        self.skipped.append(f"{name} ({why})")
        print(f"[run_tests] {name}: SKIP -- {why}")


def find_engine(explicit: str | None) -> Path | None:
    for candidate in (explicit, os.environ.get("UE_ROOT")):
        if candidate:
            path = Path(candidate)
            return path if path.is_dir() else None
    guess = REPO.parent / "UnrealEngine"
    return guess if guess.is_dir() else None


def find_godot(explicit: str | None) -> Path | None:
    for candidate in (explicit, os.environ.get("GODOT")):
        if candidate:
            path = Path(candidate)
            return path if path.is_file() else None
    found = shutil.which("godot")
    if found:
        return Path(found)
    for guess in GODOT_GUESSES:
        if guess.is_file():
            return guess
    return None


def run(name: str, argv: list[str], results: Results, cwd: Path | None = None,
        require_line: str | None = None, require_all: list[str] | None = None) -> bool:
    """Runs a test binary, echoing its own per-case lines. Exit code decides pass or fail.

    `require_line` is for a runner that can exit 0 without having finished. Godot is one: an
    unhandled GDScript error aborts _init, so `quit(1)` is never reached and the process leaves with
    0 -- which reported a whole layer green while a third of its cases had not run. Requiring the
    summary line the suite prints last is what makes "it stopped early" a failure.
    """
    print(f"[run_tests] --- {name} ---")
    if require_line is None and require_all is None:
        completed = subprocess.run(argv, cwd=str(cwd or REPO))
        ok = completed.returncode == 0
    else:
        completed = subprocess.run(argv, cwd=str(cwd or REPO), capture_output=True, text=True,
                                   errors="replace")
        output = (completed.stdout or "") + (completed.stderr or "")
        sys.stdout.write(output)
        ok = completed.returncode == 0
        if ok and require_line is not None and require_line not in output:
            ok = False
            print(f"[run_tests] {name}: exited 0 without printing {require_line!r} -- it stopped early")
        for expected in require_all or []:
            if expected in output:
                print(f"[run_tests] {name}: said {expected!r}")
            else:
                ok = False
                print(f"[run_tests] {name}: never said {expected!r}")
    results.record(name, ok)
    return ok


def build(script: str) -> bool:
    completed = subprocess.run([sys.executable, str(REPO / "tools" / script)], cwd=str(REPO))
    return completed.returncode == 0


def run_units(results: Results, do_build: bool) -> None:
    for binary, builder in (
        ("verse_lexer_test.exe", "build_lexer_test.py"),
        ("verse_class_decl_test.exe", "build_class_decl_test.py"),
    ):
        if do_build and not build(builder):
            results.record(binary, False)
            continue
        path = REPO / "bin" / binary
        if not path.is_file():
            results.skip(binary, f"not built -- run tools/{builder}")
            continue
        run(binary, [str(path)], results)

    generator = REPO / "tests" / "verse_api_gen" / "test_gen_verse_api.py"
    if generator.is_file():
        run("test_gen_verse_api.py", [sys.executable, str(generator)], results)


def run_abi(results: Results, engine: Path | None, do_build: bool) -> None:
    if engine is None:
        results.skip("host_smoke", "no Unreal checkout -- set UE_ROOT or pass --engine")
        return

    host_dll = engine / "Engine" / "Binaries" / "Win64" / "verse_host.dll"
    if not host_dll.is_file():
        results.skip("host_smoke", f"{host_dll} not built -- run tools/build_host.py")
        return

    if do_build and not build("build_smoke.py"):
        results.record("host_smoke", False)
        return

    smoke = REPO / "bin" / "host_smoke.exe"
    if not smoke.is_file():
        results.skip("host_smoke", "not built -- run tools/build_smoke.py")
        return

    # The host must be loaded from the engine tree, never from bin/: VNI records each Verse
    # package's source directory relative to the loaded module, so a copy elsewhere compiles
    # against an empty package set and every identifier is unknown.
    run("host_smoke", [str(smoke), str(host_dll), str(engine / "Engine"), str(REPO)], results)


def stage_extension(project: Path) -> str | None:
    """Copies the built GDExtension into the test project. Returns why it could not, or None."""
    source_dir = REPO / "demo" / "addons" / "godot-verse" / "bin" / "windows-x86_64"
    editor_dll = source_dir / "godot-verse.editor.dll"
    if not editor_dll.is_file():
        return "the GDExtension is not built -- run `scons target=editor`"

    target_dir = project / "addons" / "godot-verse" / "bin" / "windows-x86_64"
    target_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(editor_dll, target_dir / editor_dll.name)
    # The .gdextension names a debug library too, and Godot refuses to load the extension at all
    # when a named library is missing -- so the editor build stands in for it rather than being
    # left absent.
    shutil.copy2(editor_dll, target_dir / "godot-verse.debug.dll")

    # Outside the editor, Godot loads extensions from this list rather than by scanning -- and the
    # editor is what normally writes it. Writing it here is what lets the test project be run
    # headless without ever having been opened.
    godot_dir = project / ".godot"
    godot_dir.mkdir(exist_ok=True)
    (godot_dir / "extension_list.cfg").write_text(
        'res://addons/godot-verse/godot-verse.gdextension\n', encoding="utf-8")
    return None


def point_at_engine(project: Path, engine: Path) -> None:
    """Rewrites the project's verse/host settings to this machine's engine checkout.

    They name an absolute path on one machine, so nothing portable can be committed; rewriting
    them here is what keeps the checked-in project.godot from being somebody's local state.
    """
    settings = project / "project.godot"
    text = settings.read_text(encoding="utf-8")
    dll = (engine / "Engine" / "Binaries" / "Win64" / "verse_host.dll").as_posix().replace("/", "\\\\")
    root = engine.as_posix().replace("/", "\\\\")
    # Lambda replacements: a backslash in a re.sub replacement string is an escape, and Godot's
    # config format wants the doubled ones through verbatim.
    text = re.sub(r'host/dll_path=".*"', lambda _: f'host/dll_path="{dll}"', text)
    text = re.sub(r'host/engine_dir=".*"', lambda _: f'host/engine_dir="{root}"', text)
    settings.write_text(text, encoding="utf-8")


def run_integration(results: Results, engine: Path | None, godot: Path | None) -> None:
    project = REPO / "tests" / "integration"
    if not (project / "project.godot").is_file():
        results.skip("integration", "tests/integration is not a Godot project")
        return
    if godot is None:
        results.skip("integration", "no Godot binary -- set GODOT or pass --godot")
        return
    if engine is None:
        results.skip("integration", "no Unreal checkout -- set UE_ROOT or pass --engine")
        return

    why = stage_extension(project)
    if why is not None:
        results.skip("integration", why)
        return
    point_at_engine(project, engine)

    # --headless opens no window. --quit-after bounds a hang: the script quits on its own, and a
    # run that has not is a failure worth seeing rather than one to wait out.
    run(
        "integration",
        [
            str(godot),
            "--headless",
            "--path", str(project),
            "--script", "res://test_main.gd",
            "--quit-after", "600",
        ],
        results,
        require_line="passed, ",
    )


# What the editor must say when a script names a member the mirror deliberately does not carry.
#
# Asserted here rather than inside the project, because ScriptLanguage exposes nothing a script can
# ask -- `_validate` is an extension virtual with no bound counterpart -- so the only way to read what
# the author would see is to read what the editor prints. R-SCN-2: a reason recorded in a report file
# in this repository is read by whoever wrote the generator and by nobody else.
COVERAGE_EXPLANATIONS = [
    "Godot has get_position, but it is reachable as the property `Position`.",
    "Godot has _enter_tree, but it is a Godot virtual.",
    "Godot has Animation.length, but it cannot be a property, so Godot's own",
]


def run_coverage_diagnostic(results: Results, engine: Path | None, godot: Path | None) -> None:
    """The R-SCN-2 diagnostic, in its own project because its one script cannot compile."""
    project = REPO / "tests" / "coverage_diagnostic"
    if not (project / "project.godot").is_file():
        results.skip("coverage_diagnostic", "tests/coverage_diagnostic is not a Godot project")
        return
    if godot is None:
        results.skip("coverage_diagnostic", "no Godot binary -- set GODOT or pass --godot")
        return
    if engine is None:
        results.skip("coverage_diagnostic", "no Unreal checkout -- set UE_ROOT or pass --engine")
        return

    why = stage_extension(project)
    if why is not None:
        results.skip("coverage_diagnostic", why)
        return
    point_at_engine(project, engine)

    run(
        "coverage_diagnostic",
        [
            str(godot),
            "--headless",
            "--path", str(project),
            "--script", "res://probe_main.gd",
            "--quit-after", "600",
        ],
        results,
        require_line="[coverage] done",
        require_all=COVERAGE_EXPLANATIONS,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--only", choices=["units", "abi", "integration"], help="run one layer")
    parser.add_argument("--build", action="store_true", help="rebuild the test binaries first")
    parser.add_argument("--engine", help="the Unreal checkout (default: UE_ROOT, then ../UnrealEngine)")
    parser.add_argument("--godot", help="the Godot binary (default: GODOT, then PATH)")
    args = parser.parse_args()

    engine = find_engine(args.engine)
    godot = find_godot(args.godot)
    results = Results()

    if args.only in (None, "units"):
        run_units(results, args.build)
    if args.only in (None, "abi"):
        run_abi(results, engine, args.build)
    if args.only in (None, "integration"):
        run_integration(results, engine, godot)
        run_coverage_diagnostic(results, engine, godot)

    print()
    print(f"[run_tests] {results.passed} passed, {results.failed} failed, {len(results.skipped)} skipped")
    for skipped in results.skipped:
        print(f"[run_tests]   skipped: {skipped}")
    sys.exit(1 if results.failed else 0)


if __name__ == "__main__":
    main()
