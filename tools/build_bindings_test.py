#!/usr/bin/env python3
"""Compiles the standalone naming-rule differential test. No godot-cpp, no SCons."""

from unit_build import build

if __name__ == "__main__":
    # /bigobj: verse_gd_api.gen.h is one 23,000-row table, and verse_bindings.cpp now includes it.
    build("build_bindings_test",
          ["src/verse_bindings.cpp", "tests/verse_bindings/verse_bindings_test.cpp"],
          ["src"], "verse_bindings_test.exe", msvc_flags=("/bigobj",))
