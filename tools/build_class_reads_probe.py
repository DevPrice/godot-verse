#!/usr/bin/env python3
"""Compiles the class-reads probe with MSVC.

Not part of run_tests.py, for tests/cooked_probe's reason: it prints what a host answers and asserts
nothing. Its use is a diff between two hosts on one cook -- see the header comment in
tests/class_reads_probe/class_reads_probe.cpp. It links vm/'s JSON parser and file reader to list a
sidecar's classes, nothing else from vm/.
"""

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
    sources = [
        repo / "tests" / "class_reads_probe" / "class_reads_probe.cpp",
        repo / "vm" / "vm_json.cpp",
        repo / "vm" / "vm_file_reader.cpp",
    ]
    include_dir = repo / "include"
    vm_dir = repo / "vm"
    out_dir = repo / "bin"
    obj_dir = out_dir / "class_reads_probe_obj"
    out_exe = out_dir / "class_reads_probe.exe"

    for source in sources:
        if not source.exists():
            print(f"error: {source} does not exist", file=sys.stderr)
            sys.exit(1)

    obj_dir.mkdir(parents=True, exist_ok=True)
    source_args = " ".join(f'"{s}"' for s in sources)

    cl_cmd = (
        f'call "{vcvars64}" && '
        f'cl /std:c++20 /EHsc /Zi /I"{include_dir}" /I"{vm_dir}" {source_args} '
        f'/Fe"{out_exe}" /Fo"{obj_dir}\\\\" /Fd"{obj_dir}\\\\"'
    )
    print(f"[build_class_reads_probe] running: {cl_cmd}")
    result = subprocess.run(cl_cmd, shell=True, cwd=str(repo))
    if result.returncode != 0:
        print(f"error: cl.exe failed with exit code {result.returncode}", file=sys.stderr)
        sys.exit(result.returncode)

    print(f"[build_class_reads_probe] built {out_exe}")


if __name__ == "__main__":
    main()
