#!/usr/bin/env python3
"""Compiles the standalone GDScript-to-Verse converter test. No godot-cpp, no SCons.

MSVC on Windows, like the other unit builders; g++ or clang++ anywhere else, because the converter
is the one unit whose whole point is portable text in and text out, and it is worth being able to
run where there is no Visual Studio.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

VSWHERE = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")

# The converter, its GDScript front end, and the three godot-cpp-free units it shares names with.
SOURCES = [
    "src/verse_gd_convert.cpp",
    "src/verse_gd_resources.cpp",
    "src/verse_gd_syntax.cpp",
    "src/verse_bindings.cpp",
    "src/verse_class_decl.cpp",
    "src/verse_lexer.cpp",
    "tests/verse_gd_convert/verse_gd_convert_test.cpp",
]


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


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


def main() -> None:
    repo = repo_root()
    sources = [repo / s for s in SOURCES]
    for path in sources:
        if not path.exists():
            print(f"error: {path} does not exist", file=sys.stderr)
            sys.exit(1)

    out_dir = repo / "bin"
    out_dir.mkdir(parents=True, exist_ok=True)
    # `.exe` everywhere, because that is the name run_tests.py looks for.
    out_exe = out_dir / "verse_gd_convert_test.exe"
    src_dir = repo / "src"

    if os.name == "nt":
        vcvars64 = find_vcvars64()
        if vcvars64 is None:
            print("error: no Visual Studio installation found", file=sys.stderr)
            sys.exit(1)
        files = " ".join(f'"{s}"' for s in sources)
        # /bigobj: verse_gd_api.gen.h is one 23,000-row table.
        command = (f'call "{vcvars64}" && cl /std:c++20 /EHsc /Zi /bigobj /I"{src_dir}" {files} '
                   f'/Fe"{out_exe}" /Fo"{out_dir}\\\\"')
        print(f"[build_gd_convert_test] running: {command}")
        result = subprocess.run(command, shell=True, cwd=str(repo))
    else:
        compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            print("error: no C++ compiler found (set CXX)", file=sys.stderr)
            sys.exit(1)
        command = [compiler, "-std=c++20", "-O1", "-g", f"-I{src_dir}", *map(str, sources), "-o", str(out_exe)]
        print(f"[build_gd_convert_test] running: {' '.join(command)}")
        result = subprocess.run(command, cwd=str(repo))

    if result.returncode != 0:
        print(f"error: the compiler failed with exit code {result.returncode}", file=sys.stderr)
        sys.exit(result.returncode)
    print(f"[build_gd_convert_test] built {out_exe}")


if __name__ == "__main__":
    main()
