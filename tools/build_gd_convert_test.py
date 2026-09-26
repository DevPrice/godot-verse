#!/usr/bin/env python3
"""Compiles the standalone GDScript-to-Verse converter test. No godot-cpp, no SCons."""

from unit_build import build

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

if __name__ == "__main__":
    # /bigobj: verse_gd_api.gen.h is one 23,000-row table.
    build("build_gd_convert_test", SOURCES, ["src"], "verse_gd_convert_test.exe",
          msvc_flags=("/bigobj",), gcc_flags=("-O1",))
