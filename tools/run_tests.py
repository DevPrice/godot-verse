#!/usr/bin/env python3
"""Runs every test a contributor can run locally, and reports pass/fail without interpretation.

R-QUAL-3. The three layers R-QUAL-1 names, in the order a failure is cheapest to read, plus `web`
(phase-7.5-design.md §10.1), which runs the vm-backend export a step further, in a browser:

  units        the lexer, the class-declaration scanner and the GDScript converter, which need neither Godot nor UE
  abi          host_smoke, which drives the whole C ABI with no Godot, and the cooker
  integration  a headless Godot with Verse scripts attached, asserting on behaviour
  export       a headless Godot export, asserting on the tree it produced
  web          a Web export on the vm backend, run in headless Chrome; gated on the export
               mechanics and the game booting, not yet on the counts it reports (T6.3)

Each layer is skipped rather than failed when what it needs is absent -- a contributor without a UE
checkout still gets the unit layer -- and a skip is reported as a skip, never as a pass.

  python tools/run_tests.py                 # everything that can run here
  python tools/run_tests.py --only export   # one layer
  python tools/run_tests.py --build         # rebuild the test binaries first

Environment: UE_ROOT names the Unreal checkout (or --engine), GODOT names the Godot binary (or
--godot). Both are also guessed from the usual places. UE_ROOT is exported into every Godot this
script launches, which is how the extension finds the host and the cooker -- nothing is written
into a project.godot any more (R-DIST-12).
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
from gdextension import generate as generate_gdextension  # noqa: E402

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
        require_line: str | None = None, require_all: list[str] | None = None,
        refute_all: list[str] | None = None) -> bool:
    """Runs a test binary, echoing its own per-case lines. Exit code decides pass or fail.

    `require_line` is for a runner that can exit 0 without having finished. Godot is one: an
    unhandled GDScript error aborts _init, so `quit(1)` is never reached and the process leaves with
    0 -- which reported a whole layer green while a third of its cases had not run. Requiring the
    summary line the suite prints last is what makes "it stopped early" a failure.

    `refute_all` is the other direction, and it is what a defect in the *editor's own output* needs:
    a cyclic load and a diagnostic about a member that arrives a frame later are both things Godot
    prints and nothing returns, so the only assertion available is that the sentence is absent.
    Pair it with `require_line`, or a run that died before printing anything passes every refutation
    it was given.
    """
    print(f"[run_tests] --- {name} ---")
    if require_line is None and require_all is None and refute_all is None:
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
        for unwanted in refute_all or []:
            if unwanted in output:
                ok = False
                print(f"[run_tests] {name}: said {unwanted!r}, which it must not")
            else:
                print(f"[run_tests] {name}: never said {unwanted!r}")
    results.record(name, ok)
    return ok


def build(script: str) -> bool:
    completed = subprocess.run([sys.executable, str(REPO / "tools" / script)], cwd=str(REPO))
    return completed.returncode == 0


def run_units(results: Results, do_build: bool) -> None:
    for binary, builder in (
        ("verse_lexer_test.exe", "build_lexer_test.py"),
        ("verse_class_decl_test.exe", "build_class_decl_test.py"),
        ("verse_module_map_test.exe", "build_module_map_test.py"),
        ("verse_doc_markup_test.exe", "build_doc_markup_test.py"),
        ("verse_signature_test.exe", "build_signature_test.py"),
        ("verse_vm_test.exe", "build_vm_test.py"),
        ("verse_gd_convert_test.exe", "build_gd_convert_test.py"),
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

    run_cook(results, engine)


# Classes tests/host_smoke's fixtures declare under their own file's name, and so the classes a
# cook of them must put in the sidecar. hello.verse is not among them: it holds `Main` and no class
# at all, which is R-LANG-6's library file and is exactly what should *not* appear.
COOK_EXPECTED_CLASSES = ["debug_probe", "exports", "statics_values", "task_values", "tasks"]

# What task_values prints through a runtime host, in the order the probe calls it. Every method of
# a task value was a jump to address 0 there and nowhere else (T5.8), so the lines are the proof
# that each one ran; "[probe] done" is the proof that the process survived to say so.
RUNTIME_TASK_LINES = [
    "[verse] task_values: active",
    "[verse] task_values: not completed",
    "[verse] task_values: completed",
    "[verse] task_values: settled",
    "[verse] task_values: canceled",
    "[verse] task_values: awaited 5",
    "[verse] task_values: awaiting",
    "[verse] task_values: awaited held 9",
    "[probe] done",
]

# What HostSidecar.cpp is writing. Asserted rather than ignored because the sidecar is the one
# cooked artifact a human reads, and a version nobody bumped is how a reader-writer pair drifts.
SIDECAR_VERSION = 8


def run_cook(results: Results, engine: Path) -> None:
    """Drives verse_cook.exe over the ABI fixtures and asserts what it wrote.

    In the abi layer because that is where "the whole thing with no Godot" lives, and the cooker is
    exactly that: a compile and a save, driven by a command line. What it produces is files, so the
    assertions are here rather than in the binary -- the same reason the coverage layer's are.
    """
    cooker = engine / "Engine" / "Binaries" / "Win64" / "verse_cook.exe"
    if not cooker.is_file():
        results.skip("verse_cook", f"{cooker} not built -- run tools/build_host.py --target VerseHostCooker")
        return

    # bindings.verse is left out, and the omission is the record of what R-INT-11 still owes. Its
    # classes come from a bindings package the *consumer* hands over with vh_set_bindings, and the
    # cooker has no such call: a cook reads a manifest of files and nothing else. Until the cooked
    # path carries the bindings, cooking this fixture is four unknown identifiers.
    sources = sorted(p for p in (REPO / "tests" / "host_smoke").glob("*.verse")
                     if p.name != "bindings.verse")
    if not sources:
        results.skip("verse_cook", "tests/host_smoke has no .verse fixtures")
        return

    print("[run_tests] --- verse_cook ---")
    with tempfile.TemporaryDirectory(prefix="verse_cook_test_") as work_str:
        work = Path(work_str)
        out_dir = work / "out"

        # One source per line: absolute path, module path, res:// path -- the shape the export
        # plugin writes and §5 specifies. These fixtures are all in the root module.
        manifest = work / "sources.txt"
        manifest.write_text(
            "".join(f"{path}\t\tres://{path.name}\n" for path in sources), encoding="utf-8")

        completed = subprocess.run([str(cooker), str(manifest), str(out_dir)],
                                   capture_output=True, text=True, errors="replace")
        sys.stdout.write(completed.stdout or "")

        ok = True
        if completed.returncode != 0:
            print(f"[verse_cook] exited {completed.returncode}, not 0: FAIL")
            sys.stdout.write(completed.stderr or "")
            results.record("verse_cook", False)
            return
        print("[verse_cook] exited 0: ok")

        # One container, and the Engine/ directory reduced to the marker GForeignEngineDir needs
        # (7b D9): the loose cook is an intermediate the cooker deletes, because a `.uasset` holding
        # a Verse cell is a file nothing can load.
        for expected in ("Cooked/verse_scripts.utoc",
                         "Cooked/verse_scripts.ucas",
                         "Engine/Binaries"):
            if (out_dir / expected).exists():
                print(f"[verse_cook] wrote {expected}: ok")
            else:
                ok = False
                print(f"[verse_cook] did not write {expected}: FAIL")

        # `sources.txt` is here because everything in this directory ships: the plugin stopped
        # writing the manifest inside it, but a cache directory from before that fix kept one, and
        # kept shipping the author's absolute paths with it.
        for unwanted in ("_loose", "Cooked/global.utoc", "Engine/Content", "sources.txt",
                         "program.vbc.report.txt"):
            if (out_dir / unwanted).exists():
                ok = False
                print(f"[verse_cook] shipped {unwanted}, which is an intermediate: FAIL")
            else:
                print(f"[verse_cook] did not ship {unwanted}: ok")

        container = out_dir / "Cooked" / "verse_scripts.ucas"
        if container.stat().st_size > 1024:
            print(f"[verse_cook] the container holds {container.stat().st_size} bytes: ok")
        else:
            ok = False
            print(f"[verse_cook] the container holds {container.stat().st_size} bytes: FAIL")

        sidecar = out_dir / "verse_classes.json"
        if not sidecar.is_file():
            print("[verse_cook] wrote verse_classes.json: FAIL")
            results.record("verse_cook", False)
            return
        ok = _check_sidecar(sidecar, COOK_EXPECTED_CLASSES, "verse_cook") and ok
        ok = _check_static_tags(sidecar, "statics_values", "verse_cook") and ok
        ok = _check_vbc(out_dir / "program.vbc", "verse_cook") and ok

        results.record("verse_cook", ok)

        _run_cooked_task_values(results, engine, out_dir)


def _check_static_tags(path: Path, class_name: str, name: str) -> bool:
    """A statics constant's value carries the tag the host wrote, which for a plain float, string
    or int is none -- VH_VARIANT_NIL. Anything else is the heap the descriptor was built on (T5.7c)."""
    sidecar = json.loads(path.read_text(encoding="utf-8"))
    members = (sidecar.get("classes", {}).get(class_name, {}).get("statics") or {}).get("members", [])
    constants = [m for m in members if not m.get("isFunction")]
    if len(constants) != 3:
        print(f"[{name}] {class_name} records 3 statics constants: FAIL -- it has {len(constants)}")
        return False
    ok = True
    for member in constants:
        tag = member.get("value", {}).get("tag")
        verdict = "ok" if tag == 0 else "FAIL"
        ok = ok and tag == 0
        print(f"[{name}] {class_name}.{member.get('name')}'s value carries tag {tag}: {verdict}")
    return ok


def _run_cooked_task_values(results: Results, engine: Path, cooked: Path) -> None:
    """Runs task_values through the runtime host, which is the only host T5.8 and T5.7b showed in."""
    runtime_host = engine / "Engine" / "Binaries" / "Win64" / "verse_host_runtime.dll"
    probe = REPO / "bin" / "cooked_probe.exe"
    if not runtime_host.is_file():
        results.skip("runtime host", f"{runtime_host} not built -- run tools/build_host.py "
                     "--target VerseHostRuntime")
        return
    if not probe.is_file():
        results.skip("runtime host", "bin/cooked_probe.exe not built -- run tools/build_cooked_probe.py")
        return

    print("[run_tests] --- runtime host ---")
    completed = subprocess.run(
        [str(probe), str(runtime_host), str(cooked), str(cooked / "Cooked"), "task_values", "--frames"],
        cwd=str(REPO / "bin"), capture_output=True, text=True, errors="replace")
    output = (completed.stdout or "") + (completed.stderr or "")
    sys.stdout.write(output)
    lines = output.splitlines()

    ok = completed.returncode == 0
    print(f"[runtime host] cooked_probe exited {completed.returncode}: {'ok' if ok else 'FAIL'}")

    # In order, because a line printed by the wrong call is as wrong as a missing one.
    at = 0
    for expected in RUNTIME_TASK_LINES:
        found = next((i for i in range(at, len(lines)) if lines[i] == expected), None)
        if found is None:
            ok = False
            print(f"[runtime host] said {expected!r} in order: FAIL")
        else:
            at = found + 1
            print(f"[runtime host] said {expected!r}: ok")

    # T5.7: a runtime host delivered the message alone, and an editor host's first frame was the
    # formatter's "Callstack follows:" header.
    frames = [line for line in lines if line.startswith("[frame] ")]
    if frames:
        print(f"[runtime host] Raise's error carries {len(frames)} frame(s): ok")
    else:
        ok = False
        print("[runtime host] Raise's error carries frames: FAIL")
    if any("task_values.verse" in f and "Raise" in f for f in frames):
        print("[runtime host] a frame names Raise in task_values.verse: ok")
    else:
        ok = False
        print("[runtime host] a frame names Raise in task_values.verse: FAIL")
    if any("Callstack" in f or "follows:" in f for f in frames):
        ok = False
        print("[runtime host] no frame is a formatter's header: FAIL")
    else:
        print("[runtime host] no frame is a formatter's header: ok")

    results.record("runtime host", ok)


def _check_vbc(path: Path, name: str) -> bool:
    """The cook's .vbc reads end to end with the clean-room reader, which shares no code with the
    cooker's writer -- so a pass means the two agree on the format, not just that one is consistent."""
    if not path.is_file():
        print(f"[{name}] wrote {path.name}: FAIL")
        return False
    sys.path.insert(0, str(REPO / "tools"))
    import vbc_dump  # noqa: E402
    try:
        program = vbc_dump.load_program(path)
    except vbc_dump.VbcError as error:
        print(f"[{name}] {path.name} reads: FAIL ({error})")
        return False
    problems = vbc_dump.validate(program, path.name)
    for problem in problems[:20]:
        print(f"[{name}] {problem}")
    ok = not problems
    print(f"[{name}] {path.name} holds {len(program.cells)} cells and validates: "
          f"{'ok' if ok else 'FAIL'}")
    return ok


def _check_sidecar(path: Path, expected_classes: list[str], name: str) -> bool:
    """The sidecar parses, is the version this build reads, and names the classes it should."""
    try:
        sidecar = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        print(f"[{name}] verse_classes.json does not parse: FAIL -- {error}")
        return False

    ok = True
    for field in ("abi", "hostId", "engineCommit", "generation"):
        if sidecar.get(field) in (None, "", "unknown"):
            ok = False
            print(f"[{name}] verse_classes.json carries a {field}: FAIL")
    if ok:
        print(f"[{name}] verse_classes.json carries the build stamp: ok")

    if sidecar.get("packages"):
        print(f"[{name}] verse_classes.json names {len(sidecar['packages'])} cooked package(s): ok")
    else:
        ok = False
        print(f"[{name}] verse_classes.json names the cooked packages: FAIL")

    if sidecar.get("version") == SIDECAR_VERSION:
        print(f"[{name}] verse_classes.json is version {SIDECAR_VERSION}: ok")
    else:
        ok = False
        print(f"[{name}] verse_classes.json is version {sidecar.get('version')!r}, "
              f"not {SIDECAR_VERSION}: FAIL")

    classes = sidecar.get("classes", {})
    for expected in expected_classes:
        entry = classes.get(expected)
        if entry is None:
            ok = False
            print(f"[{name}] the sidecar holds {expected}: FAIL -- it has {sorted(classes)}")
        elif not entry.get("published"):
            # A class the cook analysed but did not publish would answer vh_has_class false, and a
            # game would find the script attachable and empty.
            ok = False
            print(f"[{name}] {expected} is published: FAIL")
        else:
            print(f"[{name}] the sidecar holds {expected}, published, "
                  f"{len(entry.get('methods', []))} method(s): ok")
    return ok


def stage_extension(project: Path, for_export: bool = False) -> str | None:
    """Copies the built GDExtension into the test project. Returns why it could not, or None."""
    source_dir = REPO / "demo" / "addons" / "godot-verse" / "bin" / "windows-x86_64"
    editor_dll = source_dir / "godot-verse.editor.dll"
    if not editor_dll.is_file():
        return "the GDExtension is not built -- run `scons target=editor`"

    target_dir = project / "addons" / "godot-verse" / "bin" / "windows-x86_64"
    target_dir.mkdir(parents=True, exist_ok=True)
    # demo/ is where scons installs, so staging *into* demo/ is a copy onto itself -- which
    # Windows reports as WinError 32, "used by another process", the same sentence an open
    # editor produces. Every tool that takes a --project was therefore unable to aim at the
    # one project this repository treats as the worked example.
    if editor_dll.resolve() != (target_dir / editor_dll.name).resolve():
        shutil.copy2(editor_dll, target_dir / editor_dll.name)
    # The .gdextension names a debug library too, and Godot refuses to load the extension at all
    # when a named library is missing -- so the editor build stands in for it rather than being
    # left absent.
    shutil.copy2(editor_dll, target_dir / "godot-verse.debug.dll")

    # An export needs the release library and the runtime host as well, because what it is
    # exporting *is* them: the [dependencies] rows that put the host beside the game are generated
    # from what is on disk, so a project staged with the editor library alone exports a game with
    # no Verse in it and the layer would be asserting against its own staging.
    if for_export:
        for name in ("godot-verse.dll", "verse_host_runtime.dll", "tbbmalloc.dll"):
            source = source_dir / name
            if not source.is_file():
                return f"{name} is not staged in demo/addons -- see the export layer's prerequisites"
            shutil.copy2(source, target_dir / name)
        generate_gdextension(str(project / "addons" / "godot-verse"),
                             str(REPO / "godot-verse.gdextension.in"))

    # Outside the editor, Godot loads extensions from this list rather than by scanning -- and the
    # editor is what normally writes it. Writing it here is what lets the test project be run
    # headless without ever having been opened.
    godot_dir = project / ".godot"
    godot_dir.mkdir(exist_ok=True)
    (godot_dir / "extension_list.cfg").write_text(
        'res://addons/godot-verse/godot-verse.gdextension\n', encoding="utf-8")
    return None


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
        require_all=[
            # R-DIAG-3. test_main.gd raises the same error twelve times in one frame; the bridge
            # prints one stack and says how many it dropped, in the wording Godot uses for its own
            # throttles. Asserted here rather than in the project because the thing being tested is
            # what reaches the output log, and a script cannot read that.
            "stack trace(s) from this error were dropped",
            # The note that a raise rolls back its call, printed with each stack. GDScript keeps
            # writes made before an error, so an author needs telling that this does not.
            "The call that raised this was rolled back",
            # Everything refresh_script_warnings produces, which reached no log until it got a
            # second reporter and so was asserted nowhere -- a `_validate` warning goes to the
            # editor's gutter and stops there. One line per category rather than per sentence, and
            # the reason rather than the whole sentence, the way the coverage_diagnostic ones are.
            #
            # R-EXP-2, an export the inspector cannot draw:
            "is an option around a value the inspector has no empty slot for",
            # B19 Stage C, an export it draws and cannot save:
            "can be assigned in the inspector but not saved",
            # R-SIG-1, a signal declaration Godot is never told about:
            "is a `var`, and a signal is an identity rather than a value",
            # R-SIG-1's other half: `@export_signal` is what registers a member, so a `signal(t)`
            # without it is listed and refused rather than skipped. The one reject whose fixture is
            # well formed in every other way, which is what makes it the test of the rule rather
            # than of the ladder above it.
            "carries no `@export_signal`, so Godot is never told about it",
            # R-EXP-9, an `@rpc` whose words Godot does not know:
            "is not an @rpc word",
            # R-EXP-1, an inspector hint on a type it cannot describe:
            "which describes an `int` -- and this member is not one",
            # R-INT-10, a Verse class extending the binding for a *script* class. The one
            # inheritance case that cannot work, and the only one the compiler is happy with:
            # `extends_binding.verse` compiles, so nothing but this sentence says it is wrong.
            "which is the generated binding for a class a *script* declares",
        ],
        # The bridge reports every Verse runtime error itself, rate limited. Unreal's own echo of
        # the same error, stack and all on every repeat, is silenced in vh_init's -LogCmds; the
        # raises test_main.gd makes on purpose are what would print one if it came back.
        refute_all=["LogVerseRuntime:"],
    )


# What the editor must say when a script names a member the mirror deliberately does not carry.
#
# Asserted here rather than inside the project, because ScriptLanguage exposes nothing a script can
# ask -- `_validate` is an extension virtual with no bound counterpart -- so the only way to read what
# the author would see is to read what the editor prints. R-SCN-2: a reason recorded in a report file
# in this repository is read by whoever wrote the generator and by nobody else.
COVERAGE_EXPLANATIONS = [
    "Godot has get_position, but it is reachable as the property `Position`.",
    # `virtual_no_default` has no row to assert: every one of Godot's 1413 virtuals is generated
    # now, because the two return kinds that had nothing to answer with both got one -- an object
    # return is an option defaulting to `false`, and a parametric container has its generated maker.
    # The reason is still in the generator and in the explanation, so a future Godot type with no
    # default reappears here rather than going quiet.
    "Godot has VisualShaderNodeFloatParameter.max, but a Verse function already answers to that "
    "name, so it is the property `Maximum`.",
    "Godot has VisualShaderNodeFloatParameter.get_max, but it is reachable as the property `Maximum`.",
    "Godot has Object.to_string, but it is reachable as `ToString(Value)`",
    # R-SCN-5's other half, and the phase's one deliberate break: the enum is the type, so the
    # integer that used to compile does not.
    "This assignment expects a value of type node_process_mode, but the assigned value is an "
    "incompatible value of type type{2}.",
    # The four module diagnostics (phase-3-design.md section 2.4). Each is asserted on the part of
    # the sentence that says *why*, not on the file names, whose order is the filesystem's.
    '"my-stuff" is not a Verse module name',
    'gameplay.vmodule, not my-stuff.vmodule',
    "both declare `collide` in the root module",
    'choose "Make Verse Module"',
    "both register the Godot class name `Widget`",
    "ClassDB is one flat namespace and a module is deliberately not part of it",
    "derives from `widget`, and more than one script answers to that name",
    "it offers Node as this script's base type",
    # The `no_rollback` trap, which the bridge no longer annotates: the appended sentence was
    # keyed on glitch 3512 and the callee's package alone, never on which effect had been refused,
    # so a `suspends` refusal from a Godot signal took the `transacts` branch and told the author
    # to write the one word an awaiting body may not carry (by-hand-findings.md B7). What is
    # asserted now is the compiler's own text, which is the whole of what the editor shows.
    "`(/user@localhost/effects:)Helper`) that has the 'no_rollback' effect, which is not allowed "
    "by its context",
    "`(/Godot.org/Godot/node:)QueueFree`) that has the 'transacts' effect, which is not allowed "
    "by its context",
    # R-TOOL-12's sentence, in both shapes: the one module that would fix it, and the two that
    # leave the choice to the author.
    "It is declared in a module this file does not import; add "
    "`using { /user@localhost/solo }` at the top of the file.",
    "It is declared in more than one module, so which was meant is yours to say",
    # A compiler warning, pinned to its severity: the build logs a warning as a warning, where an
    # analysis logs nothing at any severity. Every diagnostic is filed through one sink now, so a
    # warning that leaked around it would print as this line without the prefix.
    "WARNING: res://scripts/probe.verse:39:9: Unreachable code - previous expression is guaranteed "
    "to exit early.",
    # B19 Stage B: `@global_class` on a class that is not the one named after its file. The
    # attribute is accepted by the compiler and registers nothing, which the bridge used to pass
    # over in silence. Asserted on the reason rather than on the whole sentence, the way the module
    # ones are. The editor also puts this on the attribute's line through `_validate`; only the
    # build's copy reaches a log, which is why this is the half a test can read.
    "`@global_class` on `sidecar` registers nothing",
    "Godot collects one global class per script file",
    "still exports, filtered by its nearest Godot base class",
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


BINDING_CYCLE_REFUSALS = [
    # B30. The generator asked for a script Godot was already loading, and ResourceLoader answered
    # ERR_BUSY with nothing said -- so the only line printed names the file asked for rather than
    # the one it collided with, and reads as that file being broken.
    "Error loading resource: 'res://cycle_probe.gd'",
    # B30's neighbour. The generation that fed this build held that script back, so `cycle_probe`
    # was declared with no members and `caller.verse` failed against one that landed a frame later.
    "Unknown member `Doubled`",
    # And the consequence of the line above, which is the part an author actually pays: a build that
    # published nothing because of a diagnostic that was already false.
    "the project did not build",
]


def write_global_class_list(project: Path, entries: list[dict[str, object]]) -> None:
    """Writes the class list a headless run reads, which the editor is what normally produces.

    `ProjectSettings::get_global_class_list` loads `.godot/global_script_class_cache.cfg`
    (`project_settings.cpp:1462-1487`) and nothing outside the editor writes it, so a project that
    has never been opened reports no script classes at all -- and a binding generator with no script
    classes generates nothing, which would make this layer pass by having nothing to test. Written
    here rather than committed for the reason the `.gdextension` is: `.godot/` is Godot's to
    regenerate, and a fixture kept there is a fixture that disappears.
    """
    rows = ",\n".join(
        "{" + ",\n".join(f'"{key}": {value}' for key, value in row.items()) + "}"
        for row in entries
    )
    godot_dir = project / ".godot"
    godot_dir.mkdir(exist_ok=True)
    (godot_dir / "global_script_class_cache.cfg").write_text(
        f"list=[{rows}]\n", encoding="utf-8")


def run_binding_cycle(results: Results, engine: Path | None, godot: Path | None) -> None:
    """B30 and its neighbour, in their own project because the cycle fires during startup."""
    project = REPO / "tests" / "binding_cycle"
    if not (project / "project.godot").is_file():
        results.skip("binding_cycle", "tests/binding_cycle is not a Godot project")
        return
    if godot is None:
        results.skip("binding_cycle", "no Godot binary -- set GODOT or pass --godot")
        return
    if engine is None:
        results.skip("binding_cycle", "no Unreal checkout -- set UE_ROOT or pass --engine")
        return

    why = stage_extension(project)
    if why is not None:
        results.skip("binding_cycle", why)
        return

    write_global_class_list(project, [
        {"base": '&"Node2D"', "class": '&"CycleProbe"', "icon": '""', "is_abstract": "false",
         "is_tool": "false", "language": '&"GDScript"', "path": '"res://cycle_probe.gd"'},
        {"base": '&"Node2D"', "class": '&"Widget"', "icon": '""', "is_abstract": "false",
         "is_tool": "false", "language": '&"Verse"', "path": '"res://scripts/widget.verse"'},
    ])

    run(
        "binding_cycle",
        [
            str(godot),
            "--headless",
            "--path", str(project),
            "--script", "res://probe_main.gd",
            "--quit-after", "600",
        ],
        results,
        require_line="[cycle] done",
        require_all=["[cycle] loaded cycle_probe.gd: yes"],
        refute_all=BINDING_CYCLE_REFUSALS,
    )


# ---------------------------------------------------------------------------------- export --

PACK_HEADER_MAGIC = 0x43504447  # "GDPC"
PACK_DIR_ENCRYPTED = 1 << 0


def read_pck(path: Path) -> dict[str, int]:
    """Every file in a Godot `.pck`, as res:// path -> byte size.

    Read out of the pack rather than inferred from the export log, because the one thing this layer
    has to assert about a `.verse` is its *size*: R-DIST-11 is the claim that no source ships, and a
    one-byte stub and the whole file both read as "Storing File" in the log.

    The format is Godot's own, `core/io/file_access_pack.cpp:288-370`. Versions 2, 3 and 4 differ
    only in where the directory sits; the encrypted and sparse-bundle cases are refused rather than
    handled, because this repo's presets use neither and guessing at one would be worse than saying
    so.
    """
    raw = path.read_bytes()

    def u32(at: int) -> int:
        return struct.unpack_from("<I", raw, at)[0]

    def u64(at: int) -> int:
        return struct.unpack_from("<Q", raw, at)[0]

    if u32(0) != PACK_HEADER_MAGIC:
        raise ValueError(f"{path} does not start with GDPC")
    version = u32(4)
    if version not in (2, 3, 4):
        raise ValueError(f"{path} is pack format {version}, which this reader does not know")

    flags = u32(20)
    if flags & PACK_DIR_ENCRYPTED:
        raise ValueError(f"{path} has an encrypted directory")

    if version >= 3:
        at = u64(32)  # directory offset; pck_start_pos is 0 for a standalone .pck
    else:
        at = 24 + 8 + 16 * 4  # V2 puts the directory straight after the reserved header

    count = u32(at)
    at += 4
    files: dict[str, int] = {}
    for _ in range(count):
        length = u32(at)
        at += 4
        # The writer pads the path to four bytes and counts the padding in `length`, and it stores
        # the path with `res://` trimmed off (editor_export_platform.cpp:449). Putting the scheme
        # back is this reader's job, so a caller looks a file up by the name it would write.
        name = "res://" + raw[at:at + length].rstrip(b"\x00").decode("utf-8")
        at += length
        at += 8  # offset
        files[name] = u64(at)
        at += 8
        at += 16  # md5
        at += 4  # flags
    return files


# What tests/integration's export must carry, and what the data directory beside the executable
# must hold.
#
# Four rows shorter than 7a's: the cook's loose `.uasset` files are an intermediate now and only the
# IoStore container ships (7b D9), so there is no `<data>/Engine/Content` any more. `Engine/Binaries`
# stays, and is the only thing in `<data>/Engine`: GForeignEngineDir wants a directory with a
# `Binaries/` child and nothing else (GenericPlatformMisc.cpp:1408-1415), which is 7b's S-9 answered.
EXPORT_DATA_DIR = "verse_data"
EXPORT_BESIDE_EXE = ["godot-verse.dll", "verse_host_runtime.dll", "tbbmalloc.dll"]
EXPORT_DATA_FILES = [
    "Cooked/verse_scripts.utoc",
    "Cooked/verse_scripts.ucas",
    "Engine/Binaries",
    "verse_classes.json",
    "program.vbc",
]
# Everything the shipped data directory is allowed to hold at its top level. The export copies the
# cooker's cache directory whole, so anything else in it is shipped too.
EXPORT_DATA_DIR_ENTRIES = {"Cooked", "Engine", "verse_classes.json", "program.vbc"}
# Three of the project's own classes, one of them in a module -- the module prefix is half of a
# class's name, and it is what the kept `.vmodule` markers decide.
EXPORT_EXPECTED_CLASSES = ["marshal", "signals", "left/widget"]

# What the exported run must report, named rather than inferred (7b D5): a case that stops running
# in an export has to read as a failure and not as a shorter log. The twenty skips are
# test_cases.gd's `editor` blocks -- the second generation, the reload, `is_tool` off a stripped
# source, `get_global_name`, which is read off the same stripped source, and the hover tooltip,
# which needs an analysis a runtime host has no compiler to produce. The cases over a
# binding are no longer among them: the sidecar carries the class-to-binding table since
# R-INT-11, so an export keys a handle on a binding and every one of them runs. The five R-EXP-7 cases run the
# other way round -- only an exported game has autoloads at all, because `--script` replaces the
# main loop before Godot sets one up -- so they are skips in the editor run and passes here. The two
# runs therefore report different totals from one set of lines, and neither is a function of the
# other: the in-editor run prints 572 passed and 5 skipped against the numbers below. Adding a case
# means changing this line, which is the point of it.
EXPORT_EXPECTED_PASSES = 519
EXPORT_EXPECTED_SKIPS = 11


def run_export(results: Results, engine: Path | None, godot: Path | None) -> None:
    """Exports tests/integration headless, asserts the tree it produced, and runs it.

    The tree half is 7a's: the cook ran, its output is where D6 says, the runtime host rode along,
    the sources did not. Running it is 7b's, and is the only place anything asserts that a cooked
    Verse package loads and answers -- everything else in this suite compiles at startup.
    """
    project = REPO / "tests" / "integration"
    if godot is None:
        results.skip("export", "no Godot binary -- set GODOT or pass --godot")
        return
    if engine is None:
        results.skip("export", "no Unreal checkout -- set UE_ROOT or pass --engine")
        return
    if not (project / "export_presets.cfg").is_file():
        results.skip("export", "tests/integration has no export_presets.cfg")
        return

    cooker = engine / "Engine" / "Binaries" / "Win64" / "verse_cook.exe"
    if not cooker.is_file():
        results.skip("export", f"{cooker} not built -- run tools/build_host.py --target VerseHostCooker")
        return

    addon_bin = REPO / "demo" / "addons" / "godot-verse" / "bin" / "windows-x86_64"
    release_lib = addon_bin / "godot-verse.dll"
    if not release_lib.is_file():
        results.skip("export", "the release GDExtension is not built -- run `scons target=template_release`")
        return
    if not (addon_bin / "verse_host_runtime.dll").is_file():
        results.skip("export", "the runtime host is not staged -- run "
                               "tools/build_host.py --target VerseHostRuntime")
        return

    template = _export_template(godot)
    if template is None:
        results.skip("export", "the Windows release export template for this Godot is not installed")
        return

    why = stage_extension(project, for_export=True)
    if why is not None:
        results.skip("export", why)
        return

    print("[run_tests] --- export ---")
    with tempfile.TemporaryDirectory(prefix="verse_export_") as work_str:
        out = Path(work_str) / "game.exe"
        completed = subprocess.run(
            [str(godot), "--headless", "--path", str(project),
             "--export-release", "Windows Desktop", str(out)],
            capture_output=True, text=True, errors="replace")

        # The export log is relayed only when something is wrong with it: it is six hundred lines
        # of "Storing File" and the assertions below are what this layer is actually for.
        ok = True
        if completed.returncode != 0 or not out.is_file():
            sys.stdout.write(completed.stdout or "")
            sys.stdout.write(completed.stderr or "")
            print(f"[export] godot --export-release exited {completed.returncode}: FAIL")
            results.record("export", False)
            return
        print("[export] the export produced a game: ok")

        # add_message(EXPORT_MESSAGE_ERROR) reports without aborting (§13.4), so a clean exit code
        # is not enough -- the plugin's own errors have to be looked for.
        output = (completed.stdout or "") + (completed.stderr or "")
        verse_errors = [line for line in output.splitlines() if "ERROR: Verse:" in line]
        if verse_errors:
            ok = False
            for line in verse_errors:
                print(f"[export] the plugin reported an error: FAIL -- {line.strip()}")
        else:
            print("[export] the Verse export plugin reported no errors: ok")

        beside = out.parent
        for name in EXPORT_BESIDE_EXE:
            if (beside / name).is_file():
                print(f"[export] {name} is beside the executable: ok")
            else:
                ok = False
                print(f"[export] {name} is beside the executable: FAIL")

        data = beside / EXPORT_DATA_DIR
        if not data.is_dir():
            print(f"[export] the data directory {EXPORT_DATA_DIR!r} is beside the executable: FAIL "
                  f"-- found {sorted(p.name for p in beside.iterdir())}")
            results.record("export", False)
            return
        print("[export] the data directory is beside the executable: ok")

        for name in EXPORT_DATA_FILES:
            if (data / name).exists():
                print(f"[export] the data directory holds {name}: ok")
            else:
                ok = False
                print(f"[export] the data directory holds {name}: FAIL")

        # And nothing else, because everything in this directory ships. Asserting only what is
        # *present* let a manifest with the author's absolute paths ride along in every export for
        # as long as one cache directory had a copy (7b §13.6, and again in §13.12).
        strays = sorted(p.name for p in data.iterdir() if p.name not in EXPORT_DATA_DIR_ENTRIES)
        if strays:
            ok = False
            print(f"[export] the data directory ships nothing but the cook's output: FAIL -- {strays}")
        else:
            print("[export] the data directory ships nothing but the cook's output: ok")

        if (data / "verse_classes.json").is_file():
            ok = _check_sidecar(data / "verse_classes.json", EXPORT_EXPECTED_CLASSES, "export") and ok

        # R-DIST-11's other half, asserted rather than assumed.
        ok = _check_pck(out.with_suffix(".pck"), project) and ok

        ok = _launch_export(out) and ok

        results.record("export", ok)


def _launch_export(exe: Path) -> bool:
    """Runs the exported game and asserts what its cases reported.

    This is the only thing in the suite that exercises the cooked path end to end: the same
    `test_cases.gd` the integration layer runs in the editor, run again inside an export, where the
    project was cooked rather than compiled and the host has no compiler in it at all.

    `--fixed-fps` is not optional. Headless, the main loop runs as fast as it can and a Timer counts
    real seconds, so a case that waits on one never advances. `--verse-check` goes after `--`, where
    OS.get_cmdline_user_args() reads it and Godot's own parser cannot collide with it.
    """
    print(f"[export] launching {exe.name}")
    try:
        completed = subprocess.run(
            [str(exe), "--headless", "--fixed-fps", "60", "--", "--verse-check"],
            capture_output=True, text=True, errors="replace", timeout=600)
    except subprocess.TimeoutExpired:
        print("[export] the exported game ran for 600 s without finishing: FAIL")
        return False

    output = (completed.stdout or "") + (completed.stderr or "")
    summary = [line for line in output.splitlines() if "[integration]" in line and "passed, " in line]
    if not summary:
        sys.stdout.write(output)
        print("[export] the exported game reported no summary line: FAIL")
        return False

    print(f"[export] the exported game said: {summary[-1].strip()}")
    match = re.search(r"(\d+) passed, (\d+) failed, (\d+) skipped", summary[-1])
    if match is None:
        print("[export] the summary line does not parse: FAIL")
        return False
    passed, failed, skipped = (int(group) for group in match.groups())

    ok = True
    if completed.returncode != 0:
        # Worth printing whole: an exported game that dies has no other log.
        sys.stdout.write(output)
        print(f"[export] the exported game exited {completed.returncode}, not 0: FAIL")
        ok = False
    else:
        print("[export] the exported game exited 0: ok")

    for name, got, want in (("passed", passed, EXPORT_EXPECTED_PASSES),
                            ("failed", failed, 0),
                            ("skipped", skipped, EXPORT_EXPECTED_SKIPS)):
        if got == want:
            print(f"[export] the exported run {name} {got}: ok")
        else:
            print(f"[export] the exported run {name} {got}, expected {want}: FAIL")
            ok = False
    if not ok and failed:
        for line in output.splitlines():
            if ": FAIL" in line:
                print(f"[export]   {line.strip()}")
    return ok


def _check_pck(pck: Path, project: Path, name: str = "export") -> bool:
    """No `.verse` ships as anything but a one-byte stub, and every `.vmodule` ships whole.

    `name` is the log prefix -- "export" for the desktop layer's own pack, "web" for the same
    check against a Web export's, whose plugin path (T6.1) is otherwise identical.
    """
    if not pck.is_file():
        print(f"[{name}] {pck.name} is beside the executable: FAIL")
        return False
    try:
        files = read_pck(pck)
    except (OSError, ValueError, struct.error) as error:
        print(f"[{name}] {pck.name} parses: FAIL -- {error}")
        return False

    ok = True
    sources = sorted(files, key=str)
    verse = [entry for entry in sources if entry.endswith(".verse")]
    # Not `.godot/`: the generated bindings package lives there and is not project source. It is
    # handed to the cooker on the command line rather than shipped, so a game holds no copy of it
    # -- and Godot excludes every dot-directory from an export anyway.
    expected_verse = len([p for p in project.rglob("*.verse")
                          if not any(part.startswith(".") for part in p.relative_to(project).parts)])
    if len(verse) == expected_verse:
        print(f"[{name}] all {expected_verse} .verse files are in the pack: ok")
    else:
        ok = False
        print(f"[{name}] {len(verse)} of {expected_verse} .verse files are in the pack: FAIL")

    # One byte, which is the "\n" the plugin substitutes (D10, and C#'s ExportPlugin.cs:120-153).
    not_stubbed = [entry for entry in verse if files[entry] != 1]
    if not_stubbed:
        ok = False
        print(f"[{name}] every .verse ships as a one-byte stub: FAIL -- "
              f"{not_stubbed[0]} is {files[not_stubbed[0]]} bytes, {len(not_stubbed)} in all")
    else:
        print(f"[{name}] every .verse ships as a one-byte stub: ok")

    # The markers decide which module each script is in, and so half of every class's name. An
    # export filter that only took resources would drop them.
    # A marker is empty by design -- it names its module with its *filename*, which is why the
    # FileSystem dock needed a "Make Verse Module" item to create one at all. So this asserts the
    # entry is there and its size matches, not that the size is non-zero.
    for marker in sorted(project.rglob("*.vmodule")):
        entry = "res://" + marker.relative_to(project).as_posix()
        if entry in files and files[entry] == marker.stat().st_size:
            print(f"[{name}] {entry} ships, {files[entry]} bytes: ok")
        else:
            ok = False
            print(f"[{name}] {entry} ships: FAIL -- {files.get(entry)!r} bytes, expected "
                  f"{marker.stat().st_size}")
    return ok


# What a vm-backend export must never ship beside its executable -- the host DLL and its one
# non-system import, which nothing on that backend loads (docs/phase-7.5-design.md §9, T5.4).
EXPORT_VM_ABSENT = ["verse_host_runtime.dll", "tbbmalloc.dll"]

# How long to wait on the launched vm-backend game before giving up on it. Far short of
# _launch_export's 600 s: T4.3 (event(t)/task(t)/Sleep) is not built yet, so a case that awaits one
# hangs the game rather than failing it, and the whole suite should not pay ten minutes for that on
# every run. 90 s is comfortably past where the run above prints its last "[integration]" line.
EXPORT_VM_LAUNCH_TIMEOUT = 600


def _vm_backend_project(base_project: Path) -> Path:
    """A throwaway copy of base_project with `verse/runtime/backend` forced to "vm".

    Not an override.cfg: measured against this exact project, Godot does not honor override.cfg
    while it is running as the editor, which is what `--export-release` does -- only while running
    as the game, which is a process this function's caller has not started yet. A copy's
    project.godot is not the checked-in file CLAUDE.md's "Tests" section means by an editor-owned
    project file; it is thrown away with its temp directory by this function's caller.
    """
    work = Path(tempfile.mkdtemp(prefix="verse_export_vm_"))
    project = work / base_project.name
    shutil.copytree(base_project, project, ignore=shutil.ignore_patterns(".godot", "addons"))
    with open(project / "project.godot", "a", encoding="utf-8") as f:
        f.write('\n[verse]\n\nruntime/backend="vm"\n')
    return project


def run_export_vm(results: Results, engine: Path | None, godot: Path | None) -> None:
    """The same tests/integration export, forced onto the vm backend in a throwaway project copy.

    T5.4's own check is that the exported game ships neither host DLL -- asserted here. What the
    launched game then does is T5.1/T5.2's, not built yet, so its counts are printed rather than
    asserted and the run is recorded under its own name: a vm-backend regression must never hide
    the host-backend "export" result going red, and a vm-backend improvement must not be required
    to turn this one green before it is real (docs/web-vm/tasks.md T5.4, T5.5).
    """
    base_project = REPO / "tests" / "integration"
    if godot is None:
        results.skip("export-vm", "no Godot binary -- set GODOT or pass --godot")
        return
    if engine is None:
        results.skip("export-vm", "no Unreal checkout -- set UE_ROOT or pass --engine")
        return
    if not (base_project / "export_presets.cfg").is_file():
        results.skip("export-vm", "tests/integration has no export_presets.cfg")
        return

    cooker = engine / "Engine" / "Binaries" / "Win64" / "verse_cook.exe"
    if not cooker.is_file():
        results.skip("export-vm", f"{cooker} not built -- run tools/build_host.py --target VerseHostCooker")
        return

    addon_bin = REPO / "demo" / "addons" / "godot-verse" / "bin" / "windows-x86_64"
    if not (addon_bin / "godot-verse.dll").is_file():
        results.skip("export-vm", "the release GDExtension is not built -- run `scons target=template_release`")
        return

    template = _export_template(godot)
    if template is None:
        results.skip("export-vm", "the Windows release export template for this Godot is not installed")
        return

    project = _vm_backend_project(base_project)
    try:
        why = stage_extension(project, for_export=True)
        if why is not None:
            results.skip("export-vm", why)
            return

        print("[run_tests] --- export-vm ---")
        with tempfile.TemporaryDirectory(prefix="verse_export_vm_out_") as work_str:
            out = Path(work_str) / "game.exe"
            completed = subprocess.run(
                [str(godot), "--headless", "--path", str(project),
                 "--export-release", "Windows Desktop", str(out)],
                capture_output=True, text=True, errors="replace")

            if completed.returncode != 0 or not out.is_file():
                sys.stdout.write(completed.stdout or "")
                sys.stdout.write(completed.stderr or "")
                print(f"[export-vm] godot --export-release exited {completed.returncode}: FAIL")
                results.record("export-vm", False)
                return
            print("[export-vm] the export produced a game: ok")

            output = (completed.stdout or "") + (completed.stderr or "")
            verse_errors = [line for line in output.splitlines() if "ERROR: Verse:" in line]
            ok = True
            if verse_errors:
                ok = False
                for line in verse_errors:
                    print(f"[export-vm] the plugin reported an error: FAIL -- {line.strip()}")
            else:
                print("[export-vm] the Verse export plugin reported no errors: ok")

            beside = out.parent
            for name in EXPORT_VM_ABSENT:
                if (beside / name).exists():
                    ok = False
                    print(f"[export-vm] {name} is beside the executable, but the vm backend should ship neither: FAIL")
                else:
                    print(f"[export-vm] {name} is not shipped: ok")

            data = beside / EXPORT_DATA_DIR
            if data.is_dir():
                print("[export-vm] the data directory is beside the executable: ok")
            else:
                ok = False
                print(f"[export-vm] the data directory {EXPORT_DATA_DIR!r} is beside the executable: FAIL")

            print(f"[export-vm] launching {out.name}")
            try:
                launched = subprocess.run(
                    [str(out), "--headless", "--fixed-fps", "60", "--", "--verse-check"],
                    capture_output=True, text=True, errors="replace", timeout=EXPORT_VM_LAUNCH_TIMEOUT)
                launch_output = (launched.stdout or "") + (launched.stderr or "")
                if launched.returncode == 0:
                    print("[export-vm] the exported game exited 0: ok")
                else:
                    ok = False
                    print(f"[export-vm] the exported game exited {launched.returncode}, not 0: FAIL")
            except subprocess.TimeoutExpired as timeout_error:
                stdout = timeout_error.stdout or b""
                stderr = timeout_error.stderr or b""
                launch_output = (stdout.decode("utf-8", "replace") if isinstance(stdout, bytes) else stdout) + \
                    (stderr.decode("utf-8", "replace") if isinstance(stderr, bytes) else stderr)
                ok = False
                print(f"[export-vm] the exported game did not exit within {EXPORT_VM_LAUNCH_TIMEOUT} s: FAIL")

            # The same named counts the host backend must report: the interpreter is held to the
            # UE host's answer, case for case.
            summary = [line for line in launch_output.splitlines() if "[integration]" in line and "passed, " in line]
            match = re.search(r"(\d+) passed, (\d+) failed, (\d+) skipped", summary[-1]) if summary else None
            if match is None:
                ok = False
                print("[export-vm] the exported game reported no summary line: FAIL -- its last output:")
                for line in [line for line in launch_output.splitlines() if line.strip()][-8:]:
                    print(f"[export-vm]   {line.strip()}")
            else:
                print(f"[export-vm] the exported game said: {summary[-1].strip()}")
                for name, got, want in (("passed", int(match.group(1)), EXPORT_EXPECTED_PASSES),
                                        ("failed", int(match.group(2)), 0),
                                        ("skipped", int(match.group(3)), EXPORT_EXPECTED_SKIPS)):
                    if got == want:
                        print(f"[export-vm] {name} {got}: ok")
                    else:
                        ok = False
                        print(f"[export-vm] {name} {got}, expected {want}: FAIL")

            results.record("export-vm", ok)
    finally:
        shutil.rmtree(project.parent, ignore_errors=True)


def _export_template_dir(godot: Path) -> Path | None:
    """The export_templates directory matching this Godot's own version, or None.

    Named by the version `godot --version` prints, because a template directory for the wrong
    version produces an export that is not the one under test. Shared by every platform's template
    lookup, which differ only in which file they expect inside this directory.
    """
    completed = subprocess.run([str(godot), "--version"], capture_output=True, text=True,
                               errors="replace")
    printed = (completed.stdout or "").strip().splitlines()
    if not printed:
        return None
    # "4.7.stable.official.5b4e0cb0f" -> "4.7.stable", and with a patch number
    # "4.7.2.stable.official.ed1daf0bf" -> "4.7.2.stable". The directory is named for the status as
    # well as the number, so a patch release needs the fourth component and a .0 release does not.
    parts = printed[-1].split(".")
    if len(parts) < 3:
        return None
    version = ".".join(parts[:4]) if parts[2].isdigit() else ".".join(parts[:3])
    appdata = os.environ.get("APPDATA")
    if not appdata:
        return None
    return Path(appdata) / "Godot" / "export_templates" / version


def _export_template(godot: Path) -> Path | None:
    """The Windows release template matching this Godot, or None."""
    template_dir = _export_template_dir(godot)
    if template_dir is None:
        return None
    template = template_dir / "windows_release_x86_64.exe"
    return template if template.is_file() else None


def _web_export_template(godot: Path) -> Path | None:
    """The Web release template matching this Godot, or None.

    `dlink` is the variant that supports loading a GDExtension at all (godot-verse.nothreads.wasm
    as a side module); `nothreads` matches the preset's `variant/thread_support=false` and the
    library `tools/emsdk_env.py`'s scons line above builds (`threads=no`). A mismatched pair exports
    a Web build Godot cannot load the extension into, silently.
    """
    template_dir = _export_template_dir(godot)
    if template_dir is None:
        return None
    template = template_dir / "web_dlink_nothreads_release.zip"
    return template if template.is_file() else None


# ------------------------------------------------------------------------------------- web --

WEB_CHROME_PATH = Path("C:/Program Files/Google/Chrome/Application/chrome.exe")

# The web export tree tools/run_web.py serves: an entry page, the compiled game and its data pack.
# Not the exact filenames beyond that -- a dlink template also writes a side wasm module and worker
# scripts whose names are Godot's own to change -- so this only asserts the three that every Web
# export must produce and that a contributor recognises.
WEB_EXPORT_REQUIRED = ["index.html", "index.pck"]

# What tests/integration's export_presets.cfg carries beside "Windows Desktop" -- nothing. A Web
# preset is appended to a throwaway copy's own export_presets.cfg instead of committed here, the
# way _vm_backend_project appends `verse/runtime/backend` to a copy's project.godot rather than to
# the checked-in one (CLAUDE.md: the editor rewrites and strips comments from both files, so what
# is committed stays minimal). variant/extensions_support is what lets a Web export load a
# GDExtension at all; variant/thread_support=false matches the nothreads library this layer builds
# and requires (T0.1's dlink_nothreads template). Its include_filter is deliberately empty, as a
# freshly added preset's is: the export plugin must ship the `.vmodule` markers itself, and an
# empty filter is what proves it does, where "Windows Desktop"'s `*.vmodule` covers the other path.
WEB_PRESET_TEXT = """
[preset.1]

name="Web"
platform="Web"
runnable=true
advanced_options=false
dedicated_server=false
custom_features=""
export_filter="all_resources"
include_filter=""
exclude_filter=""
export_path=""
encryption_include_filters=""
encryption_exclude_filters=""
seed=0
encrypt_pck=false
encrypt_directory=false
script_export_mode=2

[preset.1.options]

custom_template/debug=""
custom_template/release=""
variant/extensions_support=true
variant/thread_support=false
vram_texture_compression/for_desktop=true
vram_texture_compression/for_mobile=false
html/export_icon=false
html/custom_html_shell=""
html/head_include=""
html/canvas_resize_policy=2
html/focus_canvas_on_start=true
html/experimental_virtual_keyboard=false
progressive_web_app/enabled=false
progressive_web_app/ensure_cross_origin_isolation_headers=true
progressive_web_app/offline_page=""
progressive_web_app/display=1
progressive_web_app/orientation=0
progressive_web_app/icon_144x144=""
progressive_web_app/icon_180x180=""
progressive_web_app/icon_512x512=""
progressive_web_app/background_color=Color(0, 0, 0, 1)
threads/emscripten_pool_size=8
threads/godot_pool_size=4
"""

# How long to wait for the browser to print the summary line. Generous, because a case awaiting a
# task or an event the interpreter does not yet deliver hangs the game rather than failing it.
WEB_LAUNCH_TIMEOUT = 300.0

# The exported desktop run's arguments (_launch_export) less `--headless`, which a browser page has
# no use for. run_web.py writes them into the served page's GODOT_CONFIG.
WEB_GAME_ARGS = ["--fixed-fps", "60", "--", "--verse-check"]
# The export layer's counts, less R-ASYNC-8's two thread cases: a nothreads build runs a pool task
# on the calling thread, so test_cases.gd skips them there.
WEB_EXPECTED_PASSES = EXPORT_EXPECTED_PASSES - 2
WEB_EXPECTED_SKIPS = EXPORT_EXPECTED_SKIPS + 2


def _web_backend_project(base_project: Path) -> Path:
    """A throwaway copy of base_project with a Web preset appended.

    tests/integration's checked-in export_presets.cfg carries only "Windows Desktop" (CLAUDE.md's
    export_presets.cfg rule), and Web needs its own preset to export against at all. The copy sets
    no `verse/runtime/backend*` on purpose: the `.web` override VerseRuntime registers defaults Web
    to "vm", and a copy that set it would stop proving that. Thrown away with its temp directory by this
    function's caller, exactly as _vm_backend_project's copy is.
    """
    work = Path(tempfile.mkdtemp(prefix="verse_export_web_"))
    project = work / base_project.name
    shutil.copytree(base_project, project, ignore=shutil.ignore_patterns(".godot", "addons"))
    with open(project / "export_presets.cfg", "a", encoding="utf-8") as f:
        f.write(WEB_PRESET_TEXT)
    return project


def stage_extension_web(project: Path) -> str | None:
    """Stages the GDExtension for a Web export: the Windows editor library the exporting process
    itself runs as, plus the Web library the export ships.

    Not stage_extension(for_export=True): that stages Windows's release library and the runtime
    host beside it, which a Web export needs none of and which would give generate_gdextension no
    reason to write a `web.wasm32.single.nothreads.release` row at all -- the exporting Godot is
    still the Windows editor binary either way, so it still needs its own editor library staged to
    run build_project() and invoke the cooker.
    """
    source_dir = REPO / "demo" / "addons" / "godot-verse" / "bin" / "windows-x86_64"
    editor_dll = source_dir / "godot-verse.editor.dll"
    if not editor_dll.is_file():
        return "the GDExtension is not built -- run `scons target=editor`"

    editor_target = project / "addons" / "godot-verse" / "bin" / "windows-x86_64"
    editor_target.mkdir(parents=True, exist_ok=True)
    if editor_dll.resolve() != (editor_target / editor_dll.name).resolve():
        shutil.copy2(editor_dll, editor_target / editor_dll.name)
    shutil.copy2(editor_dll, editor_target / "godot-verse.debug.dll")

    web_lib = REPO / "demo" / "addons" / "godot-verse" / "bin" / "web-wasm32" / "godot-verse.nothreads.wasm"
    if not web_lib.is_file():
        return ("the web library is not built -- run `python tools/emsdk_env.py -- scons "
                 "platform=web arch=wasm32 threads=no target=template_release`")
    web_target = project / "addons" / "godot-verse" / "bin" / "web-wasm32"
    web_target.mkdir(parents=True, exist_ok=True)
    shutil.copy2(web_lib, web_target / web_lib.name)

    generate_gdextension(str(project / "addons" / "godot-verse"),
                         str(REPO / "godot-verse.gdextension.in"))

    godot_dir = project / ".godot"
    godot_dir.mkdir(exist_ok=True)
    (godot_dir / "extension_list.cfg").write_text(
        'res://addons/godot-verse/godot-verse.gdextension\n', encoding="utf-8")
    return None


def _check_web_refuses_host(godot: Path, project: Path) -> bool:
    """Overrides `verse/runtime/backend.web` to "host" in the throwaway project and exports again.

    R-PLAT-4: the plugin must say why rather than ship a game that cannot boot. The refusal comes
    before the cook, so this costs a Godot start and nothing else. Run last, because it edits the
    project the main export ran against.
    """
    with open(project / "project.godot", "a", encoding="utf-8") as f:
        f.write('\n[verse]\n\nruntime/backend.web="host"\n')
    with tempfile.TemporaryDirectory(prefix="verse_export_web_host_") as work_str:
        completed = subprocess.run(
            [str(godot), "--headless", "--path", str(project),
             "--export-release", "Web", str(Path(work_str) / "index.html")],
            capture_output=True, text=True, errors="replace")
    output = (completed.stdout or "") + (completed.stderr or "")
    if "Verse needs the vm backend on Web" in output:
        print("[web] a Web export with backend.web=\"host\" is refused: ok")
        return True
    print("[web] a Web export with backend.web=\"host\" is refused: FAIL -- its last lines:")
    for line in [line for line in output.splitlines() if line.strip()][-12:]:
        print(f"[web]   {line.strip()}")
    return False


def run_web(results: Results, engine: Path | None, godot: Path | None) -> None:
    """Exports tests/integration for Web on the vm backend and runs it in headless Chrome.

    T6.2. Gated on the export mechanics and the game *booting* -- vh_init succeeding, which the
    first case tests/integration prints proves -- not on the counts test_cases.gd reports, which
    T6.3 makes a requirement once T5.1/T5.2 give the interpreter the rest of the runtime `vh_*`
    subset a Godot-hosted game needs (docs/web-vm/tasks.md T6.1/T6.2, phase-7.5-design.md §9/§10.1).
    """
    base_project = REPO / "tests" / "integration"
    if godot is None:
        results.skip("web", "no Godot binary -- set GODOT or pass --godot")
        return
    if engine is None:
        results.skip("web", "no Unreal checkout -- set UE_ROOT or pass --engine")
        return
    if not (base_project / "project.godot").is_file():
        results.skip("web", "tests/integration is not a Godot project")
        return

    cooker = engine / "Engine" / "Binaries" / "Win64" / "verse_cook.exe"
    if not cooker.is_file():
        results.skip("web", f"{cooker} not built -- run tools/build_host.py --target VerseHostCooker")
        return

    web_lib = REPO / "demo" / "addons" / "godot-verse" / "bin" / "web-wasm32" / "godot-verse.nothreads.wasm"
    if not web_lib.is_file():
        results.skip("web", "the web library is not built -- run `python tools/emsdk_env.py -- "
                             "scons platform=web arch=wasm32 threads=no target=template_release`")
        return

    if not WEB_CHROME_PATH.is_file():
        results.skip("web", f"no Chrome at {WEB_CHROME_PATH}")
        return

    template = _web_export_template(godot)
    if template is None:
        results.skip("web", "the Web dlink/nothreads release export template for this Godot is "
                             "not installed")
        return

    project = _web_backend_project(base_project)
    try:
        why = stage_extension_web(project)
        if why is not None:
            results.skip("web", why)
            return

        print("[run_tests] --- web ---")
        with tempfile.TemporaryDirectory(prefix="verse_export_web_out_") as work_str:
            out = Path(work_str) / "index.html"
            completed = subprocess.run(
                [str(godot), "--headless", "--path", str(project),
                 "--export-release", "Web", str(out)],
                capture_output=True, text=True, errors="replace")

            if completed.returncode != 0 or not out.is_file():
                sys.stdout.write(completed.stdout or "")
                sys.stdout.write(completed.stderr or "")
                print(f"[web] godot --export-release exited {completed.returncode}: FAIL")
                results.record("web", False)
                return
            print("[web] the export produced index.html: ok")

            output = (completed.stdout or "") + (completed.stderr or "")
            verse_errors = [line for line in output.splitlines() if "ERROR: Verse:" in line]
            ok = True
            if verse_errors:
                ok = False
                for line in verse_errors:
                    print(f"[web] the plugin reported an error: FAIL -- {line.strip()}")
            else:
                print("[web] the Verse export plugin reported no errors: ok")

            beside = out.parent
            for name in WEB_EXPORT_REQUIRED:
                if (beside / name).is_file():
                    print(f"[web] {name} is in the export tree: ok")
                else:
                    ok = False
                    print(f"[web] {name} is in the export tree: FAIL")

            if list(beside.glob("*.wasm")):
                print("[web] a .wasm module is in the export tree: ok")
            else:
                ok = False
                print("[web] a .wasm module is in the export tree: FAIL")

            for name in EXPORT_VM_ABSENT:
                if (beside / name).exists():
                    ok = False
                    print(f"[web] {name} shipped, but a Web export should carry no host DLL: FAIL")
                else:
                    print(f"[web] {name} is not shipped: ok")

            pck = beside / "index.pck"
            ok = _check_pck(pck, project, name="web") and ok
            if pck.is_file():
                try:
                    pck_files = read_pck(pck)
                    if "res://verse_data/verse_classes.json" in pck_files:
                        print("[web] verse_data is inside the .pck (res://verse_data): ok")
                    else:
                        ok = False
                        print("[web] verse_data is inside the .pck: FAIL -- "
                              f"found {sorted(n for n in pck_files if 'verse_data' in n)}")
                except (OSError, ValueError, struct.error) as error:
                    ok = False
                    print(f"[web] index.pck parses: FAIL -- {error}")

            print(f"[web] launching {out.name} in headless Chrome")
            try:
                # _launch_export's command line, handed over the only way a Web export takes one:
                # without `--verse-check` the autoload does nothing and the game idles with no
                # output at all, which is what this layer read as "never boots" until T6.3.
                launched = subprocess.run(
                    [sys.executable, str(REPO / "tools" / "run_web.py"), str(beside),
                     "--until", r"\d+ passed, \d+ failed, \d+ skipped",
                     "--timeout", str(WEB_LAUNCH_TIMEOUT)]
                    + [f"--godot-arg={arg}" for arg in WEB_GAME_ARGS],
                    capture_output=True, text=True, errors="replace", timeout=WEB_LAUNCH_TIMEOUT + 30)
                console_output = (launched.stdout or "") + (launched.stderr or "")
            except subprocess.TimeoutExpired as timeout_error:
                stdout = timeout_error.stdout or ""
                stderr = timeout_error.stderr or ""
                console_output = (stdout if isinstance(stdout, str) else stdout.decode("utf-8", "replace")) + \
                    (stderr if isinstance(stderr, str) else stderr.decode("utf-8", "replace"))
                print(f"[web] run_web.py did not exit within {WEB_LAUNCH_TIMEOUT + 30:.0f} s -- killed it")

            # Excludes run_web.py's own "timed out after ... waiting for '<pattern>'" line, which
            # echoes the --until regex text and would otherwise match this same substring search.
            summary = [line for line in console_output.splitlines()
                      if "passed, " in line and "skipped" in line and "run_web.py:" not in line]
            match = re.search(r"(\d+) passed, (\d+) failed, (\d+) skipped", summary[-1]) if summary else None
            if match is None:
                ok = False
                print("[web] the browser reported no summary line: FAIL -- its last console lines:")
                for line in [line for line in console_output.splitlines() if line.strip()][-12:]:
                    print(f"[web]   {line.strip()}")
            else:
                print(f"[web] the browser said: {summary[-1].strip()}")
                for name, got, want in (("passed", int(match.group(1)), WEB_EXPECTED_PASSES),
                                        ("failed", int(match.group(2)), 0),
                                        ("skipped", int(match.group(3)), WEB_EXPECTED_SKIPS)):
                    if got == want:
                        print(f"[web] {name} {got}: ok")
                    else:
                        ok = False
                        print(f"[web] {name} {got}, expected {want}: FAIL")

            ok = _check_web_refuses_host(godot, project) and ok
            results.record("web", ok)
    finally:
        shutil.rmtree(project.parent, ignore_errors=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--only", choices=["units", "abi", "integration", "export", "web"],
                        help="run one layer")
    parser.add_argument("--build", action="store_true", help="rebuild the test binaries first")
    parser.add_argument("--engine", help="the Unreal checkout (default: UE_ROOT, then ../UnrealEngine)")
    parser.add_argument("--godot", help="the Godot binary (default: GODOT, then PATH)")
    args = parser.parse_args()

    engine = find_engine(args.engine)
    godot = find_godot(args.godot)
    results = Results()

    # How every Godot this script launches finds the host, the cooker and the engine directory:
    # the settings used to be rewritten into each project.godot before every run, which committed
    # one machine's absolute paths to a checked-in file (R-DIST-12). The extension reads UE_ROOT
    # first, ahead of EditorSettings, and an editor started with `-s` has no EditorSettings at all.
    if engine is not None:
        os.environ["UE_ROOT"] = str(engine)

    if args.only in (None, "units"):
        run_units(results, args.build)
    if args.only in (None, "abi"):
        run_abi(results, engine, args.build)
    if args.only in (None, "integration"):
        run_integration(results, engine, godot)
        run_coverage_diagnostic(results, engine, godot)
        run_binding_cycle(results, engine, godot)
    if args.only in (None, "export"):
        run_export(results, engine, godot)
        run_export_vm(results, engine, godot)
    if args.only in (None, "web"):
        run_web(results, engine, godot)

    print()
    print(f"[run_tests] {results.passed} passed, {results.failed} failed, {len(results.skipped)} skipped")
    for skipped in results.skipped:
        print(f"[run_tests]   skipped: {skipped}")
    sys.exit(1 if results.failed else 0)


if __name__ == "__main__":
    main()
