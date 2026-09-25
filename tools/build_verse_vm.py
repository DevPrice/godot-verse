#!/usr/bin/env python3
"""Builds vm/ two ways: bin/verse_vm.dll (MSVC), or wasm32 objects (--wasm, em++).

    python tools/build_verse_vm.py            # bin/verse_vm.dll, exporting vh_*
    python tools/build_verse_vm.py --wasm      # every vm/*.cpp -> bin/vm_wasm_obj/*.o, no link

The DLL is built with VERSE_HOST_IMPLEMENTATION defined, which is what makes
include/verse_host_abi.h's VH_API macro export the vh_* functions under the exact names the header
declares -- a drop-in for verse_host_runtime.dll (design phase-7.5 §2). The wasm build leaves it
undefined, matching how vm/ is meant to be consumed once it is linked statically into the
GDExtension: nothing there should carry dllexport-equivalent visibility (§9).

/EHs-c- and /GR- on the DLL build are the clean-room's exception/RTTI discipline (no exceptions,
no RTTI, so the interpreter reports errors through return values on both build targets); the test
binary and the wasm object compile do not need to match that exactly, but the DLL does.

Both are optimized the way what ships is -- a template_release build is /O2 on Windows and -O3 on
the web -- because run_vm_conformance.py and cooked_probe's bench mode time this DLL, and a /Od
build measured nothing an exported game runs. The DLL carries a PDB for a sampling profiler.
"""

import argparse
import subprocess
import sys
from pathlib import Path

VSWHERE = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def find_vcvars64() -> Path:
    if not VSWHERE.exists():
        print(f"error: vswhere not found at {VSWHERE}", file=sys.stderr)
        sys.exit(1)

    result = subprocess.run(
        [str(VSWHERE), "-latest", "-property", "installationPath"],
        capture_output=True,
        text=True,
    )
    install_path = result.stdout.strip()
    if result.returncode != 0 or not install_path:
        print("error: vswhere could not find a Visual Studio installation", file=sys.stderr)
        sys.exit(1)

    vcvars64 = Path(install_path) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    if not vcvars64.exists():
        print(f"error: {vcvars64} not found", file=sys.stderr)
        sys.exit(1)
    return vcvars64


def vm_sources(repo: Path):
    return sorted((repo / "vm").glob("*.cpp"))


def build_dll(repo: Path) -> int:
    vcvars64 = find_vcvars64()
    vm_dir = repo / "vm"
    include_dir = repo / "include"
    out_dir = repo / "bin"
    out_dll = out_dir / "verse_vm.dll"

    sources = vm_sources(repo)
    if not sources:
        print(f"error: no .cpp files under {vm_dir}", file=sys.stderr)
        return 1

    out_dir.mkdir(parents=True, exist_ok=True)
    source_args = " ".join(f'"{s}"' for s in sources)

    cl_cmd = (
        f'call "{vcvars64}" && '
        f'cl /std:c++20 /EHs-c- /GR- /O2 /DNDEBUG /Zi /D VERSE_HOST_IMPLEMENTATION '
        f'/I"{include_dir}" /I"{vm_dir}" {source_args} '
        f'/LD /Fe"{out_dll}" /Fo"{out_dir}\\\\" /Fd"{out_dir / "verse_vm_objects.pdb"}" '
        f'/link /DEBUG /OPT:REF /OPT:ICF'
    )
    print(f"[build_verse_vm] running: {cl_cmd}")
    result = subprocess.run(cl_cmd, shell=True, cwd=str(repo))
    if result.returncode != 0:
        print(f"error: cl.exe failed with exit code {result.returncode}", file=sys.stderr)
        return result.returncode

    print(f"[build_verse_vm] built {out_dll}")
    return 0


def build_wasm(repo: Path) -> int:
    vm_dir = repo / "vm"
    include_dir = repo / "include"
    obj_dir = repo / "bin" / "vm_wasm_obj"

    sources = vm_sources(repo)
    if not sources:
        print(f"error: no .cpp files under {vm_dir}", file=sys.stderr)
        return 1

    obj_dir.mkdir(parents=True, exist_ok=True)
    emsdk_env = repo / "tools" / "emsdk_env.py"

    for source in sources:
        obj = obj_dir / (source.stem + ".o")
        cmd = [
            sys.executable,
            str(emsdk_env),
            "--",
            "em++",
            "-std=c++20",
            "-O3",
            "-fno-exceptions",
            "-fno-rtti",
            f"-I{include_dir}",
            f"-I{vm_dir}",
            "-c",
            str(source),
            "-o",
            str(obj),
        ]
        print(f"[build_verse_vm] running: {' '.join(cmd)}")
        result = subprocess.run(cmd, cwd=str(repo))
        if result.returncode != 0:
            print(f"error: em++ failed on {source} with exit code {result.returncode}", file=sys.stderr)
            return result.returncode

    print(f"[build_verse_vm] compiled {len(sources)} object(s) into {obj_dir}")
    return 0


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--wasm",
        action="store_true",
        help="compile vm/*.cpp to wasm32 objects with em++ instead of linking bin/verse_vm.dll",
    )
    args = parser.parse_args()

    repo = repo_root()
    sys.exit(build_wasm(repo) if args.wasm else build_dll(repo))


if __name__ == "__main__":
    main()
