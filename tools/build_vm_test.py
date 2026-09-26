#!/usr/bin/env python3
"""Compiles the vm/ unit tests. No godot-cpp, no SCons, like the lexer test beside it.

    python tools/build_vm_test.py             # bin/verse_vm_test.exe, unoptimized
    python tools/build_vm_test.py --release   # bin/verse_vm_test_release.exe, optimized
"""

import argparse

from unit_build import REPO, build


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--release",
        action="store_true",
        help="build bin/verse_vm_test_release.exe optimized, for --gc-bench numbers an optimized build sees",
    )
    args = parser.parse_args()

    vm_sources = sorted(p.relative_to(REPO).as_posix() for p in (REPO / "vm").glob("*.cpp"))
    # The release objects get a directory of their own: both builds name every object after its
    # source, and sharing bin/ would let one build link the other's.
    build("build_vm_test", [*vm_sources, "tests/vm/verse_vm_test.cpp"], ["vm", "include"],
          "verse_vm_test_release.exe" if args.release else "verse_vm_test.exe",
          obj_dir="bin/vm_test_release" if args.release else "bin", release=args.release)


if __name__ == "__main__":
    main()
