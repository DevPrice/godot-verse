#!/usr/bin/env python3
"""Compiles the vm/ unit tests with MSVC. No godot-cpp, no SCons, like the lexer test beside it."""

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


def main() -> None:
    vcvars64 = find_vcvars64()

    repo = repo_root()
    vm_dir = repo / "vm"
    include_dir = repo / "include"
    test_src = repo / "tests" / "vm" / "verse_vm_test.cpp"
    out_dir = repo / "bin"
    out_exe = out_dir / "verse_vm_test.exe"

    vm_sources = sorted(vm_dir.glob("*.cpp"))
    if not test_src.exists() or not vm_sources:
        print(f"error: {test_src} or vm/*.cpp is missing", file=sys.stderr)
        sys.exit(1)

    out_dir.mkdir(parents=True, exist_ok=True)
    source_args = " ".join(f'"{s}"' for s in [*vm_sources, test_src])

    cl_cmd = (
        f'call "{vcvars64}" && '
        f'cl /std:c++20 /EHsc /Zi /I"{vm_dir}" /I"{include_dir}" {source_args} '
        f'/Fe"{out_exe}" /Fo"{out_dir}\\\\"'
    )
    print(f"[build_vm_test] running: {cl_cmd}")
    result = subprocess.run(cl_cmd, shell=True, cwd=str(repo))
    if result.returncode != 0:
        print(f"error: cl.exe failed with exit code {result.returncode}", file=sys.stderr)
        sys.exit(result.returncode)

    print(f"[build_vm_test] built {out_exe}")


if __name__ == "__main__":
    main()
