#!/usr/bin/env python3
"""Exports dodge-the-creeps for Windows on each backend and reports what its frames cost.

    python tools/run_dtc_frames.py               # host and vm, three runs each
    python tools/run_dtc_frames.py --runs 5
    python tools/run_dtc_frames.py --backend vm

Each run is the exported game's own checks (`-- --verse-check`) with `--verse-frame-times`, which
has checks.gd print one `frame_times:` line: each frame's wall time, and the verse/verse_ms and
verse/godot_ms monitors that split it (docs/vm-performance.md §1.3). Under --fixed-fps and
--headless the loop runs flat out, so a frame's wall time is what it cost, not a 16.7 ms wait.

Reports rather than asserts, like tools/build_bench.py. Needs what the export layer needs:
`scons target=template_release`, the runtime host staged in demo/addons, verse_cook.exe, and the
Windows release template. tools/run_dtc_web.py is the web half.
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
sys.path.insert(0, str(REPO / "tools"))
import run_tests  # noqa: E402

FRAME_TIMES = re.compile(r"frame_times: (.*)$")
LAUNCH_TIMEOUT = 300


def backend_project(base: Path, backend: str) -> Path:
    """A throwaway copy, because dodge-the-creeps' project.godot is editor-owned (CLAUDE.md)."""
    work = Path(tempfile.mkdtemp(prefix=f"dtc_frames_{backend}_"))
    project = work / base.name
    shutil.copytree(base, project, ignore=shutil.ignore_patterns(".godot", "addons"))
    with open(project / "project.godot", "a", encoding="utf-8") as f:
        f.write(f'\n[verse]\n\nruntime/backend="{backend}"\n')
    return project


def measure(godot: Path, backend: str, runs: int) -> list[str] | None:
    project = backend_project(REPO / "dodge-the-creeps", backend)
    try:
        why = run_tests.stage_extension(project, for_export=True)
        if why is not None:
            print(f"[dtc-frames] {backend}: SKIP -- {why}")
            return None
        with tempfile.TemporaryDirectory(prefix=f"dtc_frames_{backend}_out_") as out_dir:
            exe = Path(out_dir) / "dodge.exe"
            exported = subprocess.run(
                [str(godot), "--headless", "--path", str(project), "--export-release", "Windows Desktop", str(exe)],
                capture_output=True, text=True, errors="replace")
            if exported.returncode != 0 or not exe.is_file():
                sys.stdout.write((exported.stdout or "") + (exported.stderr or ""))
                print(f"[dtc-frames] {backend}: the export failed")
                return None
            lines = []
            for index in range(runs):
                launched = subprocess.run(
                    [str(exe), "--headless", "--fixed-fps", "60", "--", "--verse-check", "--verse-frame-times"],
                    capture_output=True, text=True, errors="replace", timeout=LAUNCH_TIMEOUT)
                output = (launched.stdout or "") + (launched.stderr or "")
                found = [m.group(1) for m in map(FRAME_TIMES.search, output.splitlines()) if m]
                passed = "all checks passed" in output
                if not found:
                    print(f"[dtc-frames] {backend} run {index + 1}: no frame_times line; exit {launched.returncode}")
                    for line in [line for line in output.splitlines() if line.strip()][-8:]:
                        print(f"[dtc-frames]   {line.strip()}")
                    continue
                print(f"[dtc-frames] {backend} run {index + 1}: {found[-1]}{'' if passed else '  (checks FAILED)'}")
                lines.append(found[-1])
            return lines
    finally:
        shutil.rmtree(project.parent, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--backend", choices=["host", "vm"], help="only this backend")
    parser.add_argument("--engine", help="the Unreal checkout (default: UE_ROOT, then ../UnrealEngine)")
    parser.add_argument("--godot", help="the Godot binary (default: GODOT, then the usual places)")
    args = parser.parse_args()

    engine = run_tests.find_engine(args.engine)
    godot = run_tests.find_godot(args.godot)
    if engine is None or godot is None:
        print("[dtc-frames] needs an Unreal checkout (UE_ROOT) and a Godot binary (GODOT)")
        return 1
    os.environ["UE_ROOT"] = str(engine)

    for backend in [args.backend] if args.backend else ["host", "vm"]:
        measure(godot, backend, args.runs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
