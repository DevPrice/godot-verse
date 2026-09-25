#!/usr/bin/env python3
"""Exports dodge-the-creeps for Web on the vm backend and runs its 30 checks in headless Chrome.

T6.4 (docs/web-vm/tasks.md), the project's exit bar: dodge-the-creeps is the yardstick
(CLAUDE.md "Instruments, which are not tests"), and this is that yardstick run a step further, the
way tools/run_tests.py's `web` layer (T6.2/T6.3) runs tests/integration a step further than its own
`export` layer. It reuses that layer's machinery from tools/run_tests.py -- find_engine, find_godot,
_web_backend_project, stage_extension_web, _web_export_template, WEB_CHROME_PATH,
WEB_EXPORT_REQUIRED, EXPORT_VM_ABSENT, _check_pck -- none of which name tests/integration, so
nothing there had to change to be reused here.

What differs from run_tests.py's `web` layer is the other half: dodge-the-creeps/checks.gd is not
tests/integration/test_cases.gd. It prints one "ok "/"FAIL " line per check and a final
"headless_check: ..." line, never a "N passed, M failed, K skipped" summary -- so this script counts
check lines itself and requires all 30, zero of them "FAIL", and the final line to say "all checks
passed".

    python tools/run_dtc_web.py
    python tools/run_dtc_web.py --engine <UE checkout> --godot <godot binary>

Skipped, with a reason and exit 0, when Chrome, the built web library or the Web dlink/nothreads
export template is absent -- the same three things T6.2 gates on.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import run_tests  # noqa: E402

EXPECTED_CHECKS = 30

# checks.gd's own two line shapes (dodge-the-creeps/checks.gd's `check()` and the last line of
# `step()`) -- never a "N passed" summary, which is why this script cannot reuse run_web's regex.
CHECK_LINE = re.compile(r"^(ok|FAIL)\s+(.*)$")
FINAL_LINE = re.compile(r"^headless_check: (all checks passed|\d+ checks FAILED)$")

# Chrome's own console-log format wraps a page's console.log(text) as `"<text>", source: <url>
# (<line>)` under --enable-logging=stderr, quotes and all -- run_web.py hands this whole thing over
# as the "console line", unlike a native `godot` process's stdout, which is the bare text. Measured
# against a real run (a check's own detail can itself contain literal, unescaped double quotes --
# e.g. the property-list check's `["Node", "name", ...]` -- so this unwraps by anchoring both ends
# rather than by splitting on the first quote).
CONSOLE_WRAPPER = re.compile(r'^"(.*)", source: \S+ \(\d+\)$')


def unwrap_console_line(line: str) -> str:
    match = CONSOLE_WRAPPER.match(line)
    return match.group(1) if match else line

# Generous for the same reason run_tests.py's WEB_LAUNCH_TIMEOUT is: a case awaiting a task or an
# event the interpreter does not yet resume hangs the game rather than failing it.
WEB_LAUNCH_TIMEOUT = 300.0

# _launch's command line, handed to the page the only way a Web export takes one (run_web.py's
# --godot-arg). export_check.gd reads the same "--verse-check" user arg tests/integration's does.
#
# --disable-render-loop is load-bearing, not a speed-up: checks.gd's mob.tscn nodes carry a real
# VisibleOnScreenNotifier2D, and Godot's own `--headless` (every native run this project's checks
# pass under, including the vm-backend Windows export) never computes screen-visibility culling at
# all, so that notifier never fires on its own there -- checks.gd relies on this, freeing a mob only
# by the explicit `screen_exited.emit()` its own frame-numbered steps call. A Web export has no such
# headless mode: even inside headless Chrome, the wasm build always runs the real WebGL renderer, so
# a mob's own outward launch velocity can carry it off the small 480x720 viewport for real, firing
# its notifier and freeing itself out of step with checks.gd's fixed frame numbers -- observed as
# `mobs_seen` reading 0 instead of 1 at frame 345, and once as a null `.front()` at frame 205
# (`Cannot convert argument 1 from Nil to Object`), cascading into five failed checks. Passing
# --disable-render-loop stops the render server from running at all, which stops it from computing
# visibility the same way headless does, and reproduces the native runs' checks exactly across
# repeated exports.
GAME_ARGS = ["--fixed-fps", "60", "--disable-render-loop", "--", "--verse-check"]

# 349 frames at a fixed 60 fps is checks.gd's own game-time length (`step()` returns true at frame
# 349) -- 5.82 s of simulated time, decoupled from wall clock by --fixed-fps. How long the browser
# actually took to reach it is the only signal available for whether the interpreter keeps up at
# 60 fps in wasm; it cannot fail the run, because --fixed-fps is what makes the checks pass or fail
# the same way regardless of how long a frame really took.
DTC_SIMULATED_SECONDS = 349 / 60.0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--engine", help="the Unreal checkout (default: UE_ROOT, then ../UnrealEngine)")
    parser.add_argument("--godot", help="the Godot binary (default: GODOT, then PATH)")
    args = parser.parse_args()

    engine = run_tests.find_engine(args.engine)
    godot = run_tests.find_godot(args.godot)
    if engine is not None:
        os.environ["UE_ROOT"] = str(engine)

    base_project = REPO / "dodge-the-creeps"
    if godot is None:
        print("[dtc-web] SKIP -- no Godot binary; set GODOT or pass --godot")
        return 0
    if engine is None:
        print("[dtc-web] SKIP -- no Unreal checkout; set UE_ROOT or pass --engine")
        return 0
    if not (base_project / "project.godot").is_file():
        print(f"[dtc-web] SKIP -- {base_project} is not a Godot project")
        return 0

    cooker = engine / "Engine" / "Binaries" / "Win64" / "verse_cook.exe"
    if not cooker.is_file():
        print(f"[dtc-web] SKIP -- {cooker} not built; run tools/build_host.py --target VerseHostCooker")
        return 0

    web_lib = REPO / "demo" / "addons" / "godot-verse" / "bin" / "web-wasm32" / "godot-verse.nothreads.wasm"
    if not web_lib.is_file():
        print("[dtc-web] SKIP -- the web library is not built; run `python tools/emsdk_env.py -- "
              "scons platform=web arch=wasm32 threads=no target=template_release`")
        return 0

    if not run_tests.WEB_CHROME_PATH.is_file():
        print(f"[dtc-web] SKIP -- no Chrome at {run_tests.WEB_CHROME_PATH}")
        return 0

    template = run_tests._web_export_template(godot)
    if template is None:
        print("[dtc-web] SKIP -- the Web dlink/nothreads release export template for this Godot is not installed")
        return 0

    # A throwaway copy, exactly as run_tests.py's own web layer uses one: dodge-the-creeps'
    # project.godot and export_presets.cfg are editor-owned (CLAUDE.md) and never take the
    # `[verse] runtime/backend="vm"` line or the Web preset this needs.
    project = run_tests._web_backend_project(base_project)
    ok = True
    try:
        why = run_tests.stage_extension_web(project)
        if why is not None:
            print(f"[dtc-web] SKIP -- {why}")
            return 0

        print("[dtc-web] --- export ---")
        with tempfile.TemporaryDirectory(prefix="dtc_export_web_out_") as work_str:
            out = Path(work_str) / "index.html"
            completed = subprocess.run(
                [str(godot), "--headless", "--path", str(project),
                 "--export-release", "Web", str(out)],
                capture_output=True, text=True, errors="replace")

            if completed.returncode != 0 or not out.is_file():
                sys.stdout.write(completed.stdout or "")
                sys.stdout.write(completed.stderr or "")
                print(f"[dtc-web] godot --export-release exited {completed.returncode}: FAIL")
                return 1
            print("[dtc-web] the export produced index.html: ok")

            output = (completed.stdout or "") + (completed.stderr or "")
            verse_errors = [line for line in output.splitlines() if "ERROR: Verse:" in line]
            if verse_errors:
                ok = False
                for line in verse_errors:
                    print(f"[dtc-web] the plugin reported an error: FAIL -- {line.strip()}")
            else:
                print("[dtc-web] the Verse export plugin reported no errors: ok")

            beside = out.parent
            for name in run_tests.WEB_EXPORT_REQUIRED:
                if (beside / name).is_file():
                    print(f"[dtc-web] {name} is in the export tree: ok")
                else:
                    ok = False
                    print(f"[dtc-web] {name} is in the export tree: FAIL")

            if list(beside.glob("*.wasm")):
                print("[dtc-web] a .wasm module is in the export tree: ok")
            else:
                ok = False
                print("[dtc-web] a .wasm module is in the export tree: FAIL")

            for name in run_tests.EXPORT_VM_ABSENT:
                if (beside / name).exists():
                    ok = False
                    print(f"[dtc-web] {name} shipped, but a Web export should carry no host DLL: FAIL")
                else:
                    print(f"[dtc-web] {name} is not shipped: ok")

            pck = beside / "index.pck"
            ok = run_tests._check_pck(pck, project, name="dtc-web") and ok

            print(f"[dtc-web] --- launching {out.name} in headless Chrome ---")
            start = time.monotonic()
            console_output = ""
            try:
                launched = subprocess.run(
                    [sys.executable, str(REPO / "tools" / "run_web.py"), str(beside),
                     "--until", r"headless_check: ",
                     "--timeout", str(WEB_LAUNCH_TIMEOUT)]
                    + [f"--godot-arg={arg}" for arg in GAME_ARGS],
                    capture_output=True, text=True, errors="replace", timeout=WEB_LAUNCH_TIMEOUT + 30)
                console_output = (launched.stdout or "") + (launched.stderr or "")
            except subprocess.TimeoutExpired as timeout_error:
                stdout = timeout_error.stdout or ""
                stderr = timeout_error.stderr or ""
                console_output = (stdout if isinstance(stdout, str) else stdout.decode("utf-8", "replace")) + \
                    (stderr if isinstance(stderr, str) else stderr.decode("utf-8", "replace"))
                print(f"[dtc-web] run_web.py did not exit within {WEB_LAUNCH_TIMEOUT + 30:.0f} s -- killed it")
            elapsed = time.monotonic() - start

            checks_seen = 0
            failed_checks: list[str] = []
            final_line = None
            for line in console_output.splitlines():
                message = unwrap_console_line(line.strip())
                match = CHECK_LINE.match(message)
                if match:
                    checks_seen += 1
                    print(f"[dtc-web] {message}")
                    if match.group(1) == "FAIL":
                        failed_checks.append(message)
                    continue
                if FINAL_LINE.match(message):
                    final_line = message
                    print(f"[dtc-web] {message}")

            if final_line is None:
                ok = False
                print("[dtc-web] checks.gd never printed its final line: FAIL -- its last console lines:")
                for line in [line for line in console_output.splitlines() if line.strip()][-12:]:
                    print(f"[dtc-web]   {unwrap_console_line(line.strip())}")
            else:
                if checks_seen == EXPECTED_CHECKS:
                    print(f"[dtc-web] {checks_seen} checks printed: ok")
                else:
                    ok = False
                    print(f"[dtc-web] {checks_seen} checks printed, expected {EXPECTED_CHECKS}: FAIL")
                if failed_checks:
                    ok = False
                for line in failed_checks:
                    print(f"[dtc-web] FAILED: {line}")
                if "all checks passed" in final_line:
                    print(f"[dtc-web] {final_line}: ok")
                else:
                    ok = False
                    print(f"[dtc-web] {final_line}: FAIL")

            slower = elapsed / DTC_SIMULATED_SECONDS if DTC_SIMULATED_SECONDS else 0.0
            print(f"[dtc-web] browser wall-clock time: {elapsed:.1f} s for {DTC_SIMULATED_SECONDS:.2f} s "
                  f"of simulated game time at a fixed 60 fps ({slower:.1f}x) -- --fixed-fps makes the "
                  "checks above pass or fail the same way regardless of this number")

            print()
            print(f"[dtc-web] {'PASS' if ok else 'FAIL'}: {checks_seen} check lines, "
                  f"{len(failed_checks)} FAILED")
            return 0 if ok else 1
    finally:
        shutil.rmtree(project.parent, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
