"""The one compile step the standalone unit-test builders share. No godot-cpp, no SCons.

MSVC on Windows; g++ or clang++ anywhere else (or $CXX), so the units -- vm/ above all -- are
compiled by the compilers the non-Windows builds use. The binary is named `.exe` everywhere,
because that is the name run_tests.py looks for.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

VSWHERE = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")

REPO = Path(__file__).resolve().parent.parent


def find_vcvars64() -> Path | None:
    if not VSWHERE.exists():
        return None
    result = subprocess.run([str(VSWHERE), "-latest", "-property", "installationPath"],
                            capture_output=True, text=True)
    install_path = result.stdout.strip()
    if result.returncode != 0 or not install_path:
        return None
    vcvars64 = Path(install_path) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    return vcvars64 if vcvars64.exists() else None


def build(tag: str, sources: list[str], includes: list[str], exe_name: str, *,
          obj_dir: str = "bin", release: bool = False,
          msvc_flags: tuple[str, ...] = (), gcc_flags: tuple[str, ...] = ()) -> None:
    """Compiles `sources` (repo-relative) into bin/`exe_name`, exiting non-zero on any failure."""
    source_paths = [REPO / s for s in sources]
    for path in source_paths:
        if not path.exists():
            print(f"error: {path} does not exist", file=sys.stderr)
            sys.exit(1)

    out_exe = REPO / "bin" / exe_name
    out_dir = REPO / obj_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    if os.name == "nt":
        vcvars64 = find_vcvars64()
        if vcvars64 is None:
            print("error: no Visual Studio installation found", file=sys.stderr)
            sys.exit(1)
        flags = ["/std:c++20", "/EHsc", "/Zi", *(["/O2", "/DNDEBUG"] if release else []), *msvc_flags]
        includes_arg = " ".join(f'/I"{REPO / i}"' for i in includes)
        files = " ".join(f'"{s}"' for s in source_paths)
        command = (f'call "{vcvars64}" && cl {" ".join(flags)} {includes_arg} {files} '
                   f'/Fe"{out_exe}" /Fo"{out_dir}\\\\"')
        print(f"[{tag}] running: {command}")
        result = subprocess.run(command, shell=True, cwd=str(REPO))
    else:
        compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            print("error: no C++ compiler found (set CXX)", file=sys.stderr)
            sys.exit(1)
        command = [compiler, "-std=c++20", "-g", *(["-O2", "-DNDEBUG"] if release else []), *gcc_flags,
                   *(f"-I{REPO / i}" for i in includes), *map(str, source_paths), "-o", str(out_exe)]
        print(f"[{tag}] running: {' '.join(command)}")
        result = subprocess.run(command, cwd=str(REPO))

    if result.returncode != 0:
        print(f"error: the compiler failed with exit code {result.returncode}", file=sys.stderr)
        sys.exit(result.returncode)
    print(f"[{tag}] built {out_exe}")
