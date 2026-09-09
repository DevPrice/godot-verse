#!/usr/bin/env python3
"""Compiles the standalone ABI smoke test with MSVC."""

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
    src = repo / "tests" / "host_smoke" / "host_smoke.cpp"
    include_dir = repo / "include"
    out_dir = repo / "bin"
    out_exe = out_dir / "host_smoke.exe"

    if not src.exists():
        print(f"error: {src} does not exist", file=sys.stderr)
        sys.exit(1)

    out_dir.mkdir(parents=True, exist_ok=True)

    cl_cmd = (
        f'call "{vcvars64}" && '
        f'cl /std:c++20 /EHsc /Zi /I"{include_dir}" "{src}" '
        f'/Fe"{out_exe}" /Fo"{out_dir}\\\\"'
    )
    print(f"[build_smoke] running: {cl_cmd}")
    result = subprocess.run(cl_cmd, shell=True, cwd=str(repo))
    if result.returncode != 0:
        print(f"error: cl.exe failed with exit code {result.returncode}", file=sys.stderr)
        sys.exit(result.returncode)

    print(f"[build_smoke] built {out_exe}")


if __name__ == "__main__":
    main()
