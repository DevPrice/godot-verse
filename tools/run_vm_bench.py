#!/usr/bin/env python3
"""Times tests/vm_bench's workloads on the interpreter and on the UE runtime host, from one cook.

    python tools/run_vm_bench.py                 # cook if no cook is cached, then run both hosts
    python tools/run_vm_bench.py --cook          # cook again first
    python tools/run_vm_bench.py --repeats 50    # calls per method after the warm-up (default 20)
    python tools/run_vm_bench.py --vm-only       # no UE checkout: the interpreter alone
    python tools/run_vm_bench.py --wasm          # and the interpreter as WebAssembly, under Node
    python tools/run_vm_bench.py --wasm-profile  # the same, writing a V8 .cpuprofile

Reports timings rather than passing or failing, for tools/build_bench.py's reason: a threshold would
fail on a slower machine. Each workload is a zero-argument method of `vm_bench`, called through
tests/cooked_probe's --bench mode, which answers Godot with stubs -- so this measures Verse and the
boundary's Verse half, not Godot. `call` is the vh_instance_call; `tick` is the vh_tick after it,
which is where the interpreter collects garbage.

bin/verse_vm.dll must be tools/build_verse_vm.py's optimized build. The cook needs verse_cook.exe
and the UE side verse_host_runtime.dll, both under UE_ROOT (or --engine).
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import run_vm_conformance  # noqa: E402

FIXTURES_DIR = REPO / "tests" / "vm_bench"
COOK_DIR = REPO / "bin" / "vm_bench_cook"
COOKED_PROBE = REPO / "bin" / "cooked_probe.exe"
VM_DLL = REPO / "bin" / "verse_vm.dll"
WASM_DIR = REPO / "bin" / "vm_bench_wasm"
WASM_PROBE = WASM_DIR / "cooked_probe.js"
CLASS_NAME = "vm_bench"

BENCH_LINE = re.compile(
    r"^\[bench\] \(/user@localhost/vm_bench:\)(\w+) status=(\S+) call_min_us=([\d.]+) "
    r"call_median_us=([\d.]+) tick_median_us=([\d.]+) tick_max_us=([\d.]+)$")


def cook(engine: Path) -> bool:
    cooker = engine / "Engine" / "Binaries" / "Win64" / "verse_cook.exe"
    if not cooker.is_file():
        print(f"[vm_bench] {cooker} not built -- run tools/build_host.py --target VerseHostCooker")
        return False
    sources = sorted(FIXTURES_DIR.glob("*.verse"))
    if COOK_DIR.exists():
        shutil.rmtree(COOK_DIR)
    COOK_DIR.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix="vm_bench_manifest_") as work:
        manifest = Path(work) / "sources.txt"
        manifest.write_text("".join(f"{path}\t\tres://{path.name}\n" for path in sources), encoding="utf-8")
        completed = subprocess.run([str(cooker), str(manifest), str(COOK_DIR)],
                                   capture_output=True, text=True, errors="replace")
    if completed.returncode != 0:
        sys.stdout.write((completed.stdout or "") + (completed.stderr or ""))
        print(f"[vm_bench] verse_cook.exe exited {completed.returncode}")
        return False
    print(f"[vm_bench] cooked {len(sources)} file(s) -> {COOK_DIR}")
    return True


def parse(output: str, label: str) -> dict[str, tuple] | None:
    rows = {}
    for line in output.splitlines():
        match = BENCH_LINE.match(line.strip())
        if match:
            name, status, call_min, call_median, tick_median, tick_max = match.groups()
            rows[name] = (status, float(call_min), float(call_median), float(tick_median), float(tick_max))
    if not rows:
        print(f"[vm_bench] {label} reported nothing -- its output:")
        sys.stdout.write(output)
        return None
    return rows


def run(dll: Path, repeats: int) -> dict[str, tuple] | None:
    argv = [str(COOKED_PROBE), str(dll), str(COOK_DIR), str(COOK_DIR / "Cooked"), CLASS_NAME,
            "--bench", str(repeats)]
    completed = subprocess.run(argv, capture_output=True, text=True, errors="replace")
    return parse((completed.stdout or "") + (completed.stderr or ""), dll.name)


def build_wasm() -> bool:
    """The probe with vm/ linked in, as WebAssembly for Node: -O3 as a web template_release build
    is, and --profiling-funcs so `node --cpu-prof` names the interpreter's functions."""
    WASM_DIR.mkdir(parents=True, exist_ok=True)
    sources = sorted(str(path) for path in (REPO / "vm").glob("*.cpp"))
    argv = [sys.executable, str(REPO / "tools" / "emsdk_env.py"), "--", "em++", "-std=c++20", "-O3",
            "-fno-exceptions", "-fno-rtti", "-DCOOKED_PROBE_STATIC", "--profiling-funcs",
            f"-I{REPO / 'include'}", f"-I{REPO / 'vm'}", str(REPO / "tests" / "cooked_probe" / "cooked_probe.cpp"),
            *sources, "-sNODERAWFS=1", "-sALLOW_MEMORY_GROWTH=1", "-sSTACK_SIZE=8MB", "-o", str(WASM_PROBE)]
    completed = subprocess.run(argv, capture_output=True, text=True, errors="replace")
    if completed.returncode != 0:
        sys.stdout.write((completed.stdout or "") + (completed.stderr or ""))
        print("[vm_bench] em++ failed")
        return False
    print(f"[vm_bench] built {WASM_PROBE}")
    return True


def run_wasm(repeats: int, profile: bool) -> dict[str, tuple] | None:
    node_args = ["--cpu-prof", f"--cpu-prof-dir={WASM_DIR}"] if profile else []
    argv = [sys.executable, str(REPO / "tools" / "emsdk_env.py"), "--", "node", *node_args, str(WASM_PROBE),
            "-", str(COOK_DIR), str(COOK_DIR / "Cooked"), CLASS_NAME, "--bench", str(repeats)]
    completed = subprocess.run(argv, capture_output=True, text=True, errors="replace")
    return parse((completed.stdout or "") + (completed.stderr or ""), "the wasm probe")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cook", action="store_true", help="cook again even if a cook is cached")
    parser.add_argument("--repeats", type=int, default=20, help="timed calls per method (default 20)")
    parser.add_argument("--vm-only", action="store_true", help="skip the UE runtime host")
    parser.add_argument("--wasm", action="store_true",
                        help="also run the interpreter as WebAssembly under Node (built on first use)")
    parser.add_argument("--wasm-rebuild", action="store_true", help="rebuild the wasm probe first")
    parser.add_argument("--wasm-profile", action="store_true",
                        help=f"write a V8 .cpuprofile of the wasm run into {WASM_DIR}")
    parser.add_argument("--engine", help="the Unreal checkout (default: UE_ROOT, then ../UnrealEngine)")
    args = parser.parse_args()

    for required in (COOKED_PROBE, VM_DLL):
        if not required.is_file():
            print(f"[vm_bench] {required} not built")
            return 1

    engine = run_vm_conformance.find_engine(args.engine)
    if args.cook or not (COOK_DIR / "Cooked").is_dir():
        if engine is None:
            print("[vm_bench] no cook cached and no Unreal checkout to cook with -- set UE_ROOT")
            return 1
        if not cook(engine):
            return 1

    results = {}
    rows = run(VM_DLL, args.repeats)
    if rows is None:
        return 1
    results["vm"] = rows
    if args.wasm or args.wasm_profile:
        if (args.wasm_rebuild or not WASM_PROBE.is_file()) and not build_wasm():
            return 1
        rows = run_wasm(args.repeats, args.wasm_profile)
        if rows is None:
            return 1
        results["wasm"] = rows
    if not args.vm_only:
        if engine is None:
            print("[vm_bench] no Unreal checkout -- set UE_ROOT, or pass --vm-only")
            return 1
        rows = run(engine / "Engine" / "Binaries" / "Win64" / "verse_host_runtime.dll", args.repeats)
        if rows is None:
            return 1
        results["ue"] = rows

    print(f"[vm_bench] {args.repeats} timed calls per method; median microseconds of the call and of "
          "the vh_tick after it")
    labels = list(results)
    header = f"{'workload':<16}" + "".join(f"{label + ' call':>13}{label + ' tick':>11}" for label in labels)
    ratios = [(a, b) for a, b in (("wasm", "vm"), ("vm", "ue")) if a in results and b in results]
    header += "".join(f"{a + '/' + b:>9}" for a, b in ratios)
    print(header)
    for name in results["vm"]:
        line = f"{name:<16}"
        statuses = set()
        for label in labels:
            row = results[label].get(name)
            if row is None:
                line += f"{'-':>13}{'-':>11}"
                continue
            statuses.add(row[0])
            line += f"{row[2]:>13.1f}{row[3]:>11.1f}"
        for a, b in ratios:
            ra, rb = results[a].get(name), results[b].get(name)
            total_b = (rb[2] + rb[3]) if rb else 0.0
            line += f"{(ra[2] + ra[3]) / total_b:>8.1f}x" if ra and total_b > 0 else f"{'-':>9}"
        if statuses != {"VH_OK"}:
            line += "  (" + ", ".join(sorted(statuses)) + ")"
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
